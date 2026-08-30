# 💡 Smart Room Light Automation System (IoT)

**TL;DR:** An embedded C++ IoT project that transforms a standard ESP8266 microcontroller into a fully autonomous, local web server managing 10 active-low AC relay circuits. It features a responsive mobile UI, RESTful API endpoints, and a non-blocking state machine for real-time lighting animations.

---

## 🧰 Tech Stack & Skills Demonstrated
* **Languages:** C++, JavaScript, HTML5/CSS3
* **Hardware:** ESP8266 (NodeMCU), 10-Channel Relay Module, GPIO routing
* **Protocols & Networking:** HTTP/REST, mDNS (Zero-configuration networking), WebSockets/XHR polling
* **Embedded Concepts:** Flash memory web hosting, EEPROM state persistence, Asynchronous/Non-blocking timers (`millis()`), Over-The-Air (OTA) firmware updates

---

## 🚀 Engineering Highlights

* **Non-Blocking Architecture:** Replaced standard blocking delays with a custom asynchronous state machine. This allows complex lighting animations (Cycle, Heartbeat, Sequencing) to run in the background without stalling HTTP requests or crashing the web server.
* **Full-Stack Embedded Web Server:** The device serves a complete, mobile-responsive frontend UI directly from its internal flash memory. It utilizes client-side background polling (every 3 seconds) to keep multiple devices synced without overloading the microcontroller.
* **Server-Side API Routing:** Designed clear REST API endpoints to handle individual, zoned, and grouped lighting logic on the backend, minimizing network latency and frontend complexity.
* **Fault Tolerance & Memory:** Integrated EEPROM memory to persist lighting configurations across hard power cycles, alongside an automated Wi-Fi watchdog that self-heals dropped network connections.
* **Production-Ready Security:** Implemented OTA password protection and isolated sensitive network credentials into a separate `.gitignore` configuration file.

---

## 📡 REST API Architecture

| Endpoint Category | Example Endpoints | Functionality |
| :--- | :--- | :--- |
| **System Status** | `GET /status` | Returns current plain-text state of all 10 relays and active mode. |
| **Channel Control** | `GET /relay{N}/on` | Individually switches specific relay channels. |
| **Zone Grouping** | `GET /corners/toggle` | Server-side logic to group-switch specific room zones. |
| **Animation Modes** | `GET /heartbeat` | Triggers asynchronous lighting patterns. |

---

## 🛠️ Hardware & GPIO Mapping

A critical part of the hardware design was routing the 10-channel active-low relays while avoiding boot-failure pins on the ESP8266. 

| Relay Channel | NodeMCU Pin | ESP8266 GPIO | Default Polarity |
| :--- | :--- | :--- | :--- |
| **Relay 1-8** | D1 - D8, D0 | GPIO 5, 4, 0, 2, 14, 12, 13, 16 | Active LOW |
| **Relay 9 (RX)** | RX | GPIO 3 | Active LOW |
| **Relay 10 (TX)**| TX | GPIO 1 | Active LOW |

> *Hardware Note:* GPIO 1 (TX) and GPIO 3 (RX) are repurposed as relay outputs. To flash new firmware via USB, relay inputs on these pins must be temporarily disconnected.

---

## ⚙️ How to Build & Run

### Prerequisites
* [Arduino IDE](https://www.arduino.cc/en/software) with ESP8266 Board Support.
* Core Libraries: `ESP8266WiFi`, `ESP8266WebServer`, `ESP8266mDNS`, `ArduinoOTA`, `EEPROM`.

### Installation
1. Clone the repository:
  ```bash
   git clone [https://github.com/Garvit-533/smart-room-light-automation.git](https://github.com/Garvit-533/smart-room-light-automation.git)
