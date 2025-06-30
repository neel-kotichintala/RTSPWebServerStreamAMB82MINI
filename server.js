const express = require('express');
const net = require('net'); // For TCP server to receive RTSP URL from AMB82
const { spawn } = require('child_process'); // To run ffmpeg
const path = require('path');
const fs = require('fs');

const app = express();
const WEB_SERVER_PORT = 8080; // Port for your web browser access
const TCP_SERVER_PORT = 3000; // Port for AMB82 to send RTSP URL (match with Arduino sketch)

// Directory to store HLS segments
const HLS_DIR = path.join(__dirname, 'public', 'hls');

// --- Ensure HLS directory exists and is empty before starting ---
try {
    fs.mkdirSync(HLS_DIR, { recursive: true });
    console.log(`Ensured HLS directory exists: ${HLS_DIR}`);
    
    fs.readdirSync(HLS_DIR).forEach(file => {
        const filePath = path.join(HLS_DIR, file);
        if (fs.lstatSync(filePath).isFile()) { 
            fs.unlinkSync(filePath);
        }
    });
    console.log(`Cleaned existing HLS segments in: ${HLS_DIR}`);
} catch (error) {
    console.error(`Error managing HLS directory ${HLS_DIR}:`, error.message);
    if (error.code === 'ENOTDIR' || error.code === 'EEXIST') { 
        console.error("Please ensure that 'public/hls' is an empty folder, not a file, and has correct permissions.");
    }
    process.exit(1); 
}

let ffmpegProcess = null; // To keep track of the ffmpeg process
let currentRTSPUrl = null; // Store the current RTSP URL from the AMB82

// --- TCP Server for AMB82 Communication ---
const tcpServer = net.createServer((socket) => {
    const clientAddress = `${socket.remoteAddress}:${socket.remotePort}`;
    console.log(`[TCP Server] AMB82 connected from: ${clientAddress}`);

    socket.on('data', (data) => {
        const receivedData = data.toString().trim();
        console.log(`[TCP Server] Received data from ${clientAddress}: "${receivedData}"`);

        if (receivedData.startsWith('rtsp://')) {
            console.log(`[TCP Server] Detected RTSP URL: ${receivedData}`);
            currentRTSPUrl = receivedData; // Store the URL

            // Stop any existing ffmpeg process
            if (ffmpegProcess) {
                console.log("[FFmpeg] Stopping existing ffmpeg process...");
                ffmpegProcess.kill('SIGKILL'); // Force kill
                ffmpegProcess = null;
            }

            // Start new ffmpeg process to transcode
            startFFmpegTranscoding(currentRTSPUrl);

            socket.write('ACK: RTSP URL received and transcoding started!\r\n');
        } else {
            socket.write('NACK: Invalid data format. Expecting rtsp://...\r\n');
        }
    });

    socket.on('end', () => {
        console.log(`[TCP Server] AMB82 disconnected: ${clientAddress}`);
    });

    socket.on('error', (err) => {
        console.error(`[TCP Server] Socket error from ${clientAddress}:`, err.message);
    });
});

tcpServer.on('error', (err) => {
    console.error(`[TCP Server] Server error:`, err);
});

tcpServer.listen(TCP_SERVER_PORT, '0.0.0.0', () => {
    console.log(`Node.js TCP server listening for AMB82 on 0.0.0.0:${TCP_SERVER_PORT}`);
    console.log(`Make sure your laptop's firewall allows incoming connections on port ${TCP_SERVER_PORT}`);
});

// --- FFmpeg Transcoding Function ---
function startFFmpegTranscoding(rtspUrl) {
    console.log(`[FFmpeg] Attempting to start transcoding from: ${rtspUrl}`);
    console.log(`[FFmpeg] Outputting HLS to: ${HLS_DIR}`);

    const ffmpegArgs = [
        '-i', rtspUrl,
        '-c:v', 'copy', 
        '-c:a', 'aac', 
        '-b:a', '128k', 
        '-ac', '1', 
        '-ar', '44100', 
        '-f', 'hls',
        '-hls_time', '2', 
        '-hls_list_size', '3', 
        '-hls_flags', 'delete_segments', 
        '-start_number', '1',
        '-strftime', '1', 
        path.join(HLS_DIR, 'stream.m3u8') 
    ];

    ffmpegProcess = spawn('ffmpeg', ffmpegArgs);

    ffmpegProcess.stdout.on('data', (data) => {
        // console.log(`[FFmpeg stdout]: ${data}`); // Leave this commented for now
    });

    ffmpegProcess.stderr.on('data', (data) => {
        // *** CRITICAL CHANGE: Enable logging for stderr ***
        console.error(`[FFmpeg stderr]: ${data.toString()}`); 
    });

    ffmpegProcess.on('close', (code) => {
        console.log(`[FFmpeg] process exited with code ${code}`);
        ffmpegProcess = null; 
    });

    ffmpegProcess.on('error', (err) => {
        console.error(`[FFmpeg] Failed to start ffmpeg process: ${err.message}. Is ffmpeg installed and in your PATH?`);
        ffmpegProcess = null;
    });
}

// --- Express Web Server ---
app.use(express.static('public'));

app.get('/stream', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.get('/login', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'login.html'));
});

app.get('/qr', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'QRGenerate.html'));
});

// Start the Express web server
app.listen(WEB_SERVER_PORT, '0.0.0.0', () => {
    const os = require('os');
    const networkInterfaces = os.networkInterfaces();
    let localIp = 'localhost';
    for (const devName in networkInterfaces) {
        const iface = networkInterfaces[devName];
        for (let i = 0; i < iface.length; i++) {
            const alias = iface[i];
            if (alias.family === 'IPv4' && alias.address !== '127.0.0.1' && !alias.internal) {
                localIp = alias.address;
                break;
            }
        }
        if (localIp !== 'localhost') break;
    }
    console.log(`Node.js Web server running on http://${localIp}:${WEB_SERVER_PORT}/stream`);
    console.log(`Open your browser to http://${localIp}:${WEB_SERVER_PORT}/stream to view the stream.`);
    console.log(`Remember to update the AMB82 sketch with your laptop's local IP (${localIp}) and TCP port (${TCP_SERVER_PORT}).`);
    console.log(`Make sure your laptop's firewall allows incoming connections on port ${WEB_SERVER_PORT} and ${TCP_SERVER_PORT}.`);
});

