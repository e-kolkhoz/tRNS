#pragma once

// Dev-only serial command processor.
// Чтобы исключить из прода — закомментируй строку ниже или передай -DSERIAL_CONSOLE=0
#define SERIAL_CONSOLE 1

#if SERIAL_CONSOLE

class SerialConsole {
public:
    static void update();  // Вызывать из loop()

private:
    static void processCommand(const String& cmd);
    static String _buf;
};

#endif // SERIAL_CONSOLE
