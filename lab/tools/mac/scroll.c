#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
// scroll X Y LINES — move the pointer to (X, Y) and scroll by LINES
// (negative = down, as the wheel reports it), one line per event.
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "scroll x y lines\n"); return 2; }
    CGPoint p = CGPointMake(atof(argv[1]), atof(argv[2]));
    int lines = atoi(argv[3]);
    CGEventRef mv = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, mv); CFRelease(mv); usleep(100000);
    int step = lines < 0 ? -1 : 1;
    for (int i = 0; i != lines; i += step) {
        CGEventRef e = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, step);
        CGEventPost(kCGHIDEventTap, e); CFRelease(e); usleep(20000);
    }
    printf("scrolled %d\n", lines);
    return 0;
}
