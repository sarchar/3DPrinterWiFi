#include <ArduinoJson.h>       // https://github.com/bblanchon/ArduinoJson/wiki
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>  // https://learn.adafruit.com/esp8266-temperature-slash-humidity-webserver/code
#include <WiFiClient.h>
#include <WiFiUdp.h>

#include "MarlinWifi.h"

// If this is defined, Serial will be used to communicate with a PC.
// If not defined, it is treated as the connection to the RAMPS program
//#define PC_SERIAL

#define PC_SERIAL_BAUDRATE 115200
#define RAMPS_SERIAL_BAUDRATE 115200

// Set your wifi name and password here:
#define WIFI_NAME "essidname"
#define WIFI_PASSWORD "mypassword"

#define MULTICAST_ADDR  224, 1, 1, 1
#define MULTICAST_PORT  10334
#define MULTICAST_MAGIC 0xE468A9CC
#define RESPONDER_PORT  (MULTICAST_PORT+1)

#define HTTP_PORT 80
#define DECIMALS_TO_PRINT 3

// Serial buffer - 10K of memory, wee!
#define SERIAL_LINE_LENGTH       100
#define SERIAL_BUFFER_LINE_COUNT 100

ESP8266WebServer http_server(HTTP_PORT);
WiFiUDP udp_server;
WiFiUDP udp_responder;

typedef enum _SERVER_STATE {
    SERVER_STATE_CONNECTING,
    SERVER_STATE_IDLE
} SERVER_STATE;

SERVER_STATE current_state;

typedef struct _RAMPS_STATE {
    uint8_t printing_state;
    uint32_t printing_time; // in seconds
    uint8_t percent_done;
    struct {
        float x, y, z, e;
    } location;

    char serial_buffer[SERIAL_BUFFER_LINE_COUNT][SERIAL_LINE_LENGTH];
    int serial_line;
    int serial_line_position;
    int total_serial_lines;
} RAMPS_STATE;

RAMPS_STATE ramps_state;

int urldecode(char *dst, size_t n, const char *src)
{
    char a, b;
    int c = 0;

    if(n == 0) return 0;

    while (*src && n-- > 1) {
        if(*src == '%') {
            int s = strlen(src);
            if((s >= 3) && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
                if (a >= 'a') a -= 'a' - 'A';
                if (a >= 'A') a -= ('A' - 10);
                else a -= '0';

                if (b >= 'a') b -= 'a' - 'A';
                if (b >= 'A') b -= ('A' - 10);
                else b -= '0';

                *dst++ = 16*a+b;
                src += 3;
                c++;
            } else {
                // bad code, ignore it and return what we have
                break;
            }
        } else if(*src == '+') {
            *dst++ = ' ';
            src++;
            c++;
        } else {
            *dst++ = *src++;
            c++;
        }
    }
    *dst++ = '\0';
    return c;
}

void handle_status(void)
{
    StaticJsonBuffer<512> json_buffer;
    JsonObject& response = json_buffer.createObject();
    if(ramps_state.printing_state == WIFI_COMMAND_STATUS_PRINTING) {
        response["status"] = "printing";
    } else if(ramps_state.printing_state == WIFI_COMMAND_STATUS_IDLE) {
        response["status"] = "idle";
    } else if(ramps_state.printing_state == WIFI_COMMAND_STATUS_ERROR) {
        response["status"] = "error";
    } else {
        response["status"] = "unknown";
    }

    response["time"] = ramps_state.printing_time;
    response["progress"] = ramps_state.percent_done;

    JsonArray& location = response.createNestedArray("location");
    location.add(ramps_state.location.x, DECIMALS_TO_PRINT);
    location.add(ramps_state.location.y, DECIMALS_TO_PRINT);
    location.add(ramps_state.location.z, DECIMALS_TO_PRINT);
    location.add(ramps_state.location.e, DECIMALS_TO_PRINT);

    JsonObject& serial = response.createNestedObject("serial");
    serial["current_line"] = ramps_state.total_serial_lines;
    serial["line_pos"] = ramps_state.serial_line_position; // A front-end can use this to read partially received lines

    char buffer[256];
    response.printTo(buffer, sizeof(buffer));
    http_server.send(200, "application/json", buffer);
}

void handle_serial(void)
{
    uint32_t line = 0;

    if(http_server.hasArg("line")) {
        line = (uint32_t)http_server.arg("line").toInt();
    }

#ifdef PC_SERIAL
    Serial.print("Got /serial with argument line = ");
    Serial.println(line);
#endif

    http_server.sendHeader("Connection", "close");
    if(line >= ramps_state.total_serial_lines) {
        http_server.send(404, "text/plain", "<b>Not found</b>");
        return;
    }

    // Wrap
    line = line % SERIAL_BUFFER_LINE_COUNT;
    http_server.send(200, "text/plain", ramps_state.serial_buffer[line]);

#ifdef PC_SERIAL
    Serial.print("response = ");
    Serial.println(ramps_state.serial_buffer[line]);
#endif
}

void handle_send(void)
{
    http_server.sendHeader("Connection", "close");
    if(!http_server.hasArg("line")) {
        http_server.send(400, "text/plain", "<b>Missing argument</b>");
        return;
    }

    char line[256] = {0, };
    uint32_t c = urldecode(line, sizeof(line) - 1, http_server.arg("line").c_str());

#ifdef PC_SERIAL
    Serial.print("Sending to RAMPS: ");
    Serial.println(line);
#else
    Serial.write(WIFI_COMMAND_MAGIC1);
    Serial.write(WIFI_COMMAND_MAGIC2);
    Serial.write(WIFI_COMMAND_SERIAL);

    for(uint32_t i = 0; i < c; i++) {
        Serial.write(line[i]);
    }

    Serial.write(0);
#endif

    http_server.send(200, "application/json", "{\"result\": \"sent\"}");
}

#ifndef PC_SERIAL
uint32_t read_serial_uint32()
{
    uint8_t a = Serial.read();
    uint8_t b = Serial.read();
    uint8_t c = Serial.read();
    uint8_t d = Serial.read();
    return a | (b << 8) | (c << 16) | (d << 24);
}

float read_serial_float32()
{
    uint8_t buf[4];
    buf[0] = Serial.read();
    buf[1] = Serial.read();
    buf[2] = Serial.read();
    buf[3] = Serial.read();
    return *((float*)(&buf[0]));
}

uint32_t red_light_millis = 0;
void check_serial(void)
{
    uint8_t buf[4];
    uint32_t t, c;

    if(Serial.available() < 3) return;

    uint8_t magic1 = Serial.read();
    uint8_t magic2 = Serial.read();
    if(magic1 != WIFI_COMMAND_MAGIC1 || magic2 != WIFI_COMMAND_MAGIC2) {
        digitalWrite(0, LOW);
        red_light_millis = millis() + 300;
        return;
    }

    uint8_t cmd = Serial.read();
    switch(cmd) {
    case WIFI_COMMAND_STATUS:
        // Wait 20ms for the data to arrive
        t = millis() + 20;
        while( Serial.available() < 22 && t > millis() ) ;
        if(Serial.available() < 22) break;

        ramps_state.printing_state = Serial.read();
        ramps_state.printing_time = read_serial_uint32();
        ramps_state.percent_done = Serial.read();
        ramps_state.location.x = read_serial_float32();
        ramps_state.location.y = read_serial_float32();
        ramps_state.location.z = read_serial_float32();
        ramps_state.location.e = read_serial_float32();
        break;
    case WIFI_COMMAND_SERIAL:
        // Give 50ms to receiving any serial message up to 256 characters, terminating with a 0 byte
        t = millis() + 50;
        c = 0;

        while( t > millis() && c < 256 ) {
            if(!Serial.available()) continue;
            c += 1;

            uint8_t ch = Serial.read();

            // 0 byte terminates the read
            if( ch == 0 ) break;

            // Ignore \r
            if( (char)ch == '\r' ) continue;

            // If writing into a new line, increase line count (this will catch blank lines too!)
            if(ramps_state.serial_line_position == 0) ramps_state.total_serial_lines += 1;

            // Move onto the next line but don't store \n
            if( (char)ch == '\n' ) {
                ramps_state.serial_line_position = 0;
                ramps_state.serial_line = (ramps_state.serial_line + 1) % SERIAL_BUFFER_LINE_COUNT;
                continue;
            }

            // Store char
            ramps_state.serial_buffer[ramps_state.serial_line][ramps_state.serial_line_position] = (char)ch;
            ramps_state.serial_line_position += 1;
            ramps_state.serial_buffer[ramps_state.serial_line][ramps_state.serial_line_position] = 0;
            if(ramps_state.serial_line_position == (SERIAL_LINE_LENGTH - 1)) {
                // If overflowing the line, move onto the next -- but don't count it as a new line until
                // it gets at least 1 character
                ramps_state.serial_line = (ramps_state.serial_line + 1) % SERIAL_BUFFER_LINE_COUNT;
                ramps_state.serial_line_position = 0;
                ramps_state.serial_buffer[ramps_state.serial_line][0] = 0;
            }
        }

        break;
    }
}
#endif

void setup(void)
{
    memset(&ramps_state, 0, sizeof(ramps_state));

    pinMode(0, OUTPUT);
    digitalWrite(0, HIGH);

#ifdef PC_SERIAL
    Serial.begin(PC_SERIAL_BAUDRATE);
    Serial.print("\r\nStarting...\r\n");
#else
    Serial.begin(RAMPS_SERIAL_BAUDRATE);
#endif

    WiFi.begin(WIFI_NAME, WIFI_PASSWORD);

    // Configure the page listings
    http_server.on("/", [](){
        http_server.send(200, "application/json", "{\"result\": \"Hello, world!\"}");
    });

    http_server.on("/status", handle_status);
    http_server.on("/serial", handle_serial);
    http_server.on("/send", handle_send);

    http_server.on("/addline", [](){
        memcpy(ramps_state.serial_buffer[ramps_state.serial_line], "Hello, world!", 13);
        ramps_state.serial_buffer[ramps_state.serial_line][13] = 0;
        ramps_state.serial_line = (ramps_state.serial_line + 1) % SERIAL_BUFFER_LINE_COUNT;
        ramps_state.total_serial_lines += 1;
        ramps_state.serial_line_position = 0;
    });

    http_server.begin();
}

void loop(void)
{
#ifndef PC_SERIAL
    // We can receive data from RAMPS immediately
    check_serial();
#endif
 
    switch(current_state) {

    case SERVER_STATE_CONNECTING:
        if(WiFi.status() == WL_CONNECTED) {
            current_state = SERVER_STATE_IDLE;
#ifdef PC_SERIAL
            Serial.print("Connected to WiFi rounter. IP address is ");
            Serial.println(WiFi.localIP());
#endif

            // Begin listening for multicast
            udp_server.beginMulticast(WiFi.localIP(), IPAddress(MULTICAST_ADDR), MULTICAST_PORT);
        }
        break;

    case SERVER_STATE_IDLE:
        if(WiFi.status() != WL_CONNECTED) {
#ifdef PC_SERIAL
            Serial.print("Lost connection to WiFi router!\r\n");
#endif
            current_state = SERVER_STATE_CONNECTING;
            udp_responder.stop();
            break;
        }

        http_server.handleClient();

        if(udp_server.parsePacket() >= 6) {
            uint32_t magic;
            uint16_t port;
            uint8_t buffer[6];
            udp_server.read(buffer, 6);
            magic = buffer[0] | (buffer[1] << 8) | (buffer[2] << 16) | (buffer[3] << 24);
            port = buffer[4] | (buffer[5] << 8);
            if(magic == MULTICAST_MAGIC) {
                IPAddress remoteIP = udp_server.remoteIP();
#ifdef PC_SERIAL
                Serial.print("Got multicast from ");
                Serial.print(remoteIP);
                Serial.print(":");
                Serial.print(port);
                Serial.print("... ");
#endif

                // Send response
                IPAddress localIP = WiFi.localIP();
                if(udp_responder.beginPacket(remoteIP, port)) {
                    udp_responder.write((uint8_t)~(MULTICAST_MAGIC & 0xFF));
                    udp_responder.write((uint8_t)~((MULTICAST_MAGIC >>  8) & 0xFF));
                    udp_responder.write((uint8_t)~((MULTICAST_MAGIC >> 16) & 0xFF));
                    udp_responder.write((uint8_t)~((MULTICAST_MAGIC >> 24) & 0xFF));
                    udp_responder.write((uint8_t)(HTTP_PORT & 0xFF));
                    udp_responder.write((uint8_t)((HTTP_PORT >> 8) & 0xFF));
                    udp_responder.endPacket();
#ifdef PC_SERIAL
                    Serial.println("Response sent");
#endif
                } else {
#ifdef PC_SERIAL
                    Serial.println("Response failure");
#endif
                }
            }
        }
        udp_server.flush();

        break;
    }

#ifndef PC_SERIAL
    if(red_light_millis < millis()) { 
        digitalWrite(0, HIGH);
        red_light_millis = millis();
    }
#endif
}
