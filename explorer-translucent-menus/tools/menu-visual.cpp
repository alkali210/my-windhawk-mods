// Interactive visual fixture for the same production hooks, no Explorer changes.
#define main SmokeMain
#include "menu-smoke.cpp"
#undef main
int main(int argc, char** argv) {
    // Permit the same focused native-loop checks from either isolated executable.
    if (argc > 1) return SmokeMain(argc, argv);
    char name[] = "menu-visual";
    char mode[] = "--preview";
    char* args[] = {name, mode};
    return SmokeMain(2, args);
}
