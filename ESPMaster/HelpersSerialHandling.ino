extern void addToSerialLog(const String &message);
extern String currentSerialLine;

//Wrappers around Serial.print/println/printf that also mirror output into the web log buffer,
//and no-op the physical Serial calls when SERIAL_ENABLE is off.
template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
    currentSerialLine += String(value);
}

void SerialPrint(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
    currentSerialLine += value.toString();
}

template <typename T>
void SerialPrintf(const char* message, T value) {
#if SERIAL_ENABLE == true
    Serial.printf(message, value);
#endif
    char buffer[256];
    snprintf(buffer, sizeof(buffer), message, value);
    addToSerialLog(String(buffer));
}

template <typename T>
void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
    currentSerialLine += String(value);
    addToSerialLog(currentSerialLine);
    currentSerialLine = "";
}

void SerialPrintln(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
    currentSerialLine += value.toString();
    addToSerialLog(currentSerialLine);
    currentSerialLine = "";
}
