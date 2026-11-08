// Interactive visual fixture for the same production hooks, no Explorer changes.
#define main SmokeMain
#include "menu-smoke.cpp"
#undef main
int main() {
    char name[] = "menu-visual";
    char mode[] = "--preview";
    char* args[] = {name, mode};
    return SmokeMain(2, args);
}
