//Wrappers around Serial.print/println/printf, no-ops when SERIAL_ENABLE is off.
template <typename T>
void SerialPrint(T value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
}

void SerialPrint(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.print(value);
#endif
}

template <typename T>
void SerialPrintf(const char* message, T value) {
#if SERIAL_ENABLE == true
    Serial.printf(message, value);
#endif
}

//Overload for F()-wrapped (flash-resident) format strings - printf_P() is the documented-safe way
//to read a format string via PGM_P rather than plain const char*.
template <typename T>
void SerialPrintf(const __FlashStringHelper* message, T value) {
#if SERIAL_ENABLE == true
    Serial.printf_P(reinterpret_cast<PGM_P>(message), value);
#endif
}

template <typename T>
void SerialPrintln(T value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
}

void SerialPrintln(IPAddress value) {
#if SERIAL_ENABLE == true
    Serial.println(value);
#endif
}
