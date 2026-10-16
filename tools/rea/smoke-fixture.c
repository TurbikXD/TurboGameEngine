/* Owned, execution-free Ghidra smoke fixture. No CRT or imported libraries. */
__declspec(dllexport) int sound_pan(int x, int width) {
    if (width <= 0) return 0;
    int pan = (x * 200) / width - 100;
    if (pan < -100) return -100;
    if (pan > 100) return 100;
    return pan;
}

void mainCRTStartup(void) {}
