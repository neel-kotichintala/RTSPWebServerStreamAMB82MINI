#undef DEFAULT
#include "VideoStream.h"
#include "QRCodeScanner.h"
#include "WiFi.h"
#include "StreamIO.h"
#include "AudioStream.h"
#include "AudioEncoder.h"
#include "RTSP.h"
#include <WiFiClient.h> // Required for basic TCP client functionality
#include <HttpClient.h>

#define CHANNEL 0   // Channel for RTSP streaming
#define CHANNELQR 2 // Channel for QR code scanning
#define MAX_WIFI_ATTEMPTS 30
#define QR_SCAN_INTERVAL 500

// RTSP/Video/Audio settings (keep these global)
// AS PER YOUR WORKING CODE: configV initialized with CHANNELQR
VideoSetting configV(800, 600, 30, VIDEO_H264, 0); 

// Audio config: Default (0) usually means standard settings like 16kHz, mono, 16-bit
AudioSetting configA(0); 
Audio audio;
AAC aac;
RTSP rtsp;
StreamIO audioStreamer(1, 1); // Streamer for raw audio to AAC encoder
StreamIO avMixStreamer(2, 1); // RTSP AV mixer: 2 inputs (video, audio), 1 output (RTSP)

// QR Code Scanner related objects
QRCodeScanner* Scanner = nullptr; 
String lastProcessedQR = "";
unsigned long lastScanTime = 0;
bool wifiConnected = false;
bool rtspStarted = false; 
bool rtspLinkSent = false; // Flag to ensure we send the RTSP link to the server only once per stream start

// --- Server Details for TCP Communication ---
// *** IMPORTANT: REPLACE WITH YOUR LAPTOP'S ACTUAL LOCAL IP ADDRESS (e.g., 192.168.1.100) ***
const char* serverAddress = "192.168.1.232"; 
const int serverPort = 3000; // Node.js TCP server port (must match your server.js)

// --- WiFi Scan/Parse/Connect Functions ---
bool parseWiFiQR(const String& qrString, String& ssid_out, String& password_out, String& security_out) {
    if (!qrString.startsWith("WIFI:") || qrString.length() < 10) return false;
    ssid_out = ""; password_out = ""; security_out = "";
    int pos = 5; int colonPos = qrString.indexOf(':', pos);
    while (colonPos != -1) {
        int valueStart = colonPos + 1;
        int semicolonPos = qrString.indexOf(';', valueStart);
        if (semicolonPos == -1) semicolonPos = qrString.length(); 
        
        char key = qrString.charAt(pos);
        String value = qrString.substring(valueStart, semicolonPos);
        
        switch (key) {
            case 'S': ssid_out = value; break;
            case 'P': password_out = value; break;
            case 'T': security_out = value; break;
        }
        pos = semicolonPos + 1;
        if (pos >= qrString.length()) break; 
        colonPos = qrString.indexOf(':', pos);
    }
    return ssid_out.length() > 0;
}

bool connectToWiFiOptimized(const String& ssid_in, const String& password_in) {
    Serial.println("Connecting to: " + ssid_in);
    WiFi.disconnect();
    delay(100);
    char ssidBuffer[ssid_in.length() + 1];
    char passBuffer[password_in.length() + 1];
    ssid_in.toCharArray(ssidBuffer, sizeof(ssidBuffer));
    password_in.toCharArray(passBuffer, sizeof(passBuffer));
    if (password_in.length() > 0) {
        WiFi.begin(ssidBuffer, passBuffer);
    } else {
        WiFi.begin(ssidBuffer);
    }
    unsigned long startTime = millis();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < MAX_WIFI_ATTEMPTS) {
        delay(250);
        if (attempts % 4 == 0) Serial.print(".");
        attempts++;
        if (millis() - startTime > 15000) break;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✓ WiFi Connected!");
        Serial.print("IP: "); Serial.print(WiFi.localIP());
        Serial.print(" | Signal: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
        return true;
    } else {
        Serial.println("\n✗ Connection failed");
        return false;
    }
}

// *** TCP Client Function to Send RTSP Link to Node.js Server ***
void sendRTSPLinkToServer() {
    // Only send if WiFi is connected, RTSP is started, and link hasn't been sent for this stream session
    if (WiFi.status() == WL_CONNECTED && rtspStarted && !rtspLinkSent) {
        WiFiClient client;
        Serial.print("Attempting to connect to TCP server at ");
        Serial.print(serverAddress);
        Serial.print(":");
        Serial.println(serverPort);

        if (client.connect(serverAddress, serverPort)) {
            Serial.println("TCP Connected to server!");
            
            // *** CRITICAL FIX: Manually construct IP address string from bytes ***
            IPAddress ip = WiFi.localIP();
            String deviceIP = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
            
            String rtspURL = "rtsp://" + deviceIP + ":554/stream"; // Assuming default RTSP port 554 from AMB82

            Serial.print("Sending RTSP URL: ");
            Serial.println(rtspURL);

            client.print(rtspURL); // Send the RTSP URL as a string
            client.print("\r\n"); // Add newline for server to easily read

            // Optional: read response from server (e.g., "ACK: Data received!")
            String response = "";
            unsigned long timeout = millis() + 2000; // 2 second timeout for response
            while (client.connected() && !client.available() && millis() < timeout) {
                delay(10);
            }
            if (client.available()) {
                response = client.readStringUntil('\n');
                Serial.print("Server responded: ");
                response.trim(); // Trim in place
                Serial.println(response); // Then print the trimmed string
            } else {
                Serial.println("No response from server within timeout.");
            }

            client.stop(); // Close the connection
            Serial.println("TCP Connection closed.");
            rtspLinkSent = true; // Mark as sent for this session
        } else {
            Serial.println("TCP Connection to server failed.");
        }
    } else if (!rtspStarted) {
        Serial.println("RTSP stream not started yet, cannot send link.");
    } else if (rtspLinkSent) {
        Serial.println("RTSP link already sent to server for this session.");
    } else {
        Serial.println("WiFi not connected, cannot send RTSP link.");
    }
}

// --- Process QR Code (primary flow) ---
void processQRCode(const String& qrData) {
    if (qrData == lastProcessedQR) return;
    lastProcessedQR = qrData;
    Serial.println("New QR detected: " + qrData);
    String parsedSsid, parsedPassword, parsedSecurity;
    if (parseWiFiQR(qrData, parsedSsid, parsedPassword, parsedSecurity)) {
        Serial.println("Parsed - SSID: " + parsedSsid + " | Security: " + parsedSecurity);
        if (connectToWiFiOptimized(parsedSsid, parsedPassword)) {
            wifiConnected = true;
            // After successful WiFi connection, release the QR scanner
            if (Scanner) {
                Serial.println("Attempting to stop QR Scanner and release resources...");
                Serial.println("Stopping camera channel for QR scanner (CHANNELQR)...");
                Camera.channelEnd(CHANNELQR); 
                delay(200); 

                Serial.println("De-initializing camera video subsystem...");
                Camera.videoDeinit(); 
                delay(5000); // *** Significant delay (5 seconds) for hardware to fully release ***

                Serial.println("Attempting to delete QR Code Scanner object...");
                delete Scanner; 
                Scanner = nullptr; 
                Serial.println("QR Code Scanner object deleted and camera resources released.");
            }
            delay(2000); // Additional delay for system to settle
            Serial.println("System cleanup after QR scan complete. Ready for RTSP.");
        } else {
            lastProcessedQR = ""; 
            delay(2000);
        }
    } else {
        Serial.println("Invalid WiFi QR format");
        lastProcessedQR = ""; 
    }
}

// --- RTSP Stream Initialization Function ---
void startRTSPStream() {
    if (rtspStarted) {
        Serial.println("RTSP stream already started. Skipping initialization.");
        return;
    }

    Serial.println("----- Starting RTSP Stream Setup (VIDEO + AUDIO) -----");

    Serial.println("Configuring Camera Video Channel for RTSP (CHANNEL 0)...");
    configV.setBitrate(200 * 1024); 
    // Resolution and FPS are set in the global VideoSetting configV constructor (from CHANNELQR preset)
    Camera.configVideoChannel(CHANNEL, configV); // Use CHANNEL 0 for RTSP
    Serial.println("Re-initializing Camera video subsystem for RTSP (after config)...");
    Camera.videoInit();
    delay(200); 
    Serial.println("Camera Video Initialized for RTSP.");

    Serial.println("Configuring Audio Peripheral and Encoder...");
    audio.configAudio(configA); audio.begin();
    aac.configAudio(configA); aac.begin();
    Serial.println("Audio Configured.");
    
    Serial.println("Setting up StreamIO for Audio (Raw to AAC Encoder)...");
    audioStreamer.registerInput(audio); audioStreamer.registerOutput(aac);
    if (audioStreamer.begin() != 0) { 
        Serial.println("ERROR: StreamIO audio link (Raw->AAC) start failed. Aborting RTSP."); 
        Camera.channelEnd(CHANNEL);
        Camera.videoDeinit();
        return; 
    }
    Serial.println("StreamIO Audio Link OK.");
    
    Serial.println("Configuring RTSP Server...");
    rtsp.configVideo(configV); 
    rtsp.configAudio(configA, CODEC_AAC); 
    rtsp.begin();
    Serial.println("RTSP Server Initialized.");

    Serial.println("Starting Camera Channel (CHANNEL 0)...");
    Camera.channelBegin(CHANNEL); 
    delay(5000); 

    Serial.println("Setting up StreamIO for Video + Audio Mixing...");
    auto videoStream = Camera.getStream(CHANNEL); 
    
    avMixStreamer.registerInput1(videoStream); 
    avMixStreamer.registerInput2(aac); 
    avMixStreamer.registerOutput(rtsp); 

    if (avMixStreamer.begin() != 0) {
        Serial.println("ERROR: StreamIO AV mix link start failed (Video+Audio). Aborting RTSP.");
        Camera.channelEnd(CHANNEL);
        Camera.videoDeinit();
        audioStreamer.end(); 
        rtsp.end();          
        aac.end();           
        audio.end();         
        return;
    }
    Serial.println("StreamIO AV Mix Link OK (Video+Audio)."); 
    
    delay(1000); 

    Serial.println("------------------------------");
    Serial.println("- Summary of Streaming -");
    Serial.println("------------------------------");
    Camera.printInfo();
    IPAddress ip = WiFi.localIP();
    Serial.println("- RTSP -");
    Serial.print("rtsp://");
    Serial.print(ip);
    Serial.print(":");
    rtsp.printInfo(); 
    Serial.println("- Audio -"); 
    audio.printInfo();           

    rtspStarted = true;
    Serial.println("----- RTSP Stream Setup Complete (VIDEO + AUDIO). -----");
}

// --- Main Setup and Loop ---

void setup() {
    Serial.begin(115200);
    Serial.println("System Booting...");

    // Configure and initialize Camera for CHANNELQR (Channel 2) for QR scanning initially
    Camera.configVideoChannel(CHANNELQR, configV); 
    Camera.videoInit(); 

    Scanner = new QRCodeScanner();
    if (Scanner) {
        Scanner->StartScanning();
        Serial.println("QR Code WiFi Scanner Initialized and Started.");
    } else {
        Serial.println("ERROR: Failed to allocate QRCodeScanner! Halting.");
        while(1); 
    }
    
    Serial.println("Please scan a WiFi QR code to connect and start RTSP...");
}

void loop() {
    if (!wifiConnected) {
        // --- QR Scanning Loop ---
        if (Scanner) { 
            unsigned long currentTime = millis();
            if (currentTime - lastScanTime >= QR_SCAN_INTERVAL) {
                Scanner->GetResultString();
                
                if (Scanner->ResultString != nullptr && strlen(Scanner->ResultString) > 0) {
                    processQRCode(String(Scanner->ResultString));
                }
                
                lastScanTime = currentTime;
            }
        }
        delay(50); 
        
    } else { // WiFi is connected
        if (!rtspStarted) {
            startRTSPStream();
            // *** Call NEW FUNCTION: Send the RTSP link to the Node.js server after stream starts ***
            sendRTSPLinkToServer(); 
        }

        // --- WiFi and RTSP Monitoring Loop ---
        static unsigned long lastCheck = 0;
        if (millis() - lastCheck > 10000) { // Check every 10 seconds
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi disconnected! Stopping RTSP and restarting QR scanner...");
                
                // Stop RTSP components in reverse order of setup
                avMixStreamer.end();        
                Camera.channelEnd(CHANNEL); 
                rtsp.end();                 
                audioStreamer.end();
                aac.end();
                audio.end();
                delay(200); 

                Serial.println("De-initializing camera video subsystem before re-starting for QR scan...");
                Camera.videoDeinit(); 
                delay(5000); 

                Camera.configVideoChannel(CHANNELQR, configV); // Re-initialize QR channel (CHANNELQR)
                Camera.videoInit();                          
                delay(1000); 

                rtspStarted = false;        
                wifiConnected = false;      
                lastProcessedQR = "";       
                rtspLinkSent = false; // Reset flag so link is sent again on reconnect

                // Re-allocate and restart QR scanner for re-connection
                if (!Scanner) { 
                    Serial.println("Re-allocating QR Code Scanner...");
                    Scanner = new QRCodeScanner();
                    if (Scanner) {
                        Scanner->StartScanning();
                        Serial.println("QR Code WiFi Scanner re-started.");
                    } else {
                        Serial.println("ERROR: Failed to re-allocate QRCodeScanner! Cannot re-scan.");
                    }
                }
            } else {
                Serial.print("WiFi OK - IP: ");
                Serial.println(WiFi.localIP());
            }
            lastCheck = millis();
        }
        delay(1000); 
    }
}