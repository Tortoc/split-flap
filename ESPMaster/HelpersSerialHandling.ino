//Wrappers around Serial.print/println/printf, no-op'd entirely when SERIAL_ENABLE is off. Used
//to previously also mirror output into a web-readable log ring buffer (removed 2026-09-08 to free
//RAM - see BUGFIX_LOG #125 - it cost ~5KB static RAM for a rarely-used diagnostic endpoint on this
//chronically heap-constrained device); this is now a thin, allocation-free wrapper only.
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
