// Function to add message to serial log buffer (declared in ESPMaster.ino)
extern void addToSerialLog(String message);
extern String currentSerialLine;

//Offers option to call these methods to do serial output and not have to do 
//definition checks with every call
template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
    // Always log to web interface buffer (build line)
    currentSerialLine += String(value);
}

// Explicit overload for IPAddress
void SerialPrint(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
    // Always log to web interface buffer (build line)
    currentSerialLine += value.toString();
}

template <typename T>
void SerialPrintf(const char* message, T value) {
#if SERIAL_ENABLE == true
    Serial.printf(message, value);
#endif
    // Always log to web interface buffer
    char buffer[256];
    snprintf(buffer, sizeof(buffer), message, value);
    addToSerialLog(String(buffer));
}

template <typename T>
void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
    // Always log to web interface buffer (complete line)
    currentSerialLine += String(value);
    addToSerialLog(currentSerialLine);
    currentSerialLine = ""; // Clear for next line
}

// Explicit overload for IPAddress
void SerialPrintln(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
    // Always log to web interface buffer (complete line)
    currentSerialLine += value.toString();
    addToSerialLog(currentSerialLine);
    currentSerialLine = ""; // Clear for next line
}
