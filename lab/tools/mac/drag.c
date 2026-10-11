#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
static void post(CGEventType t, CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e); CFRelease(e);
}
int main(int argc, char** argv) {
    if (argc < 6) { fprintf(stderr, "drag x0 y0 x1 y1 ms\n"); return 2; }
    double x0 = atof(argv[1]), y0 = atof(argv[2]), x1 = atof(argv[3]), y1 = atof(argv[4]);
    int ms = atoi(argv[5]), steps = ms / 10;
    post(kCGEventMouseMoved, CGPointMake(x0, y0)); usleep(150000);
    post(kCGEventLeftMouseDown, CGPointMake(x0, y0)); usleep(100000);
    for (int i = 1; i <= steps; ++i) {
        double t = (double)i / steps;
        post(kCGEventLeftMouseDragged, CGPointMake(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
        usleep(10000);
    }
    usleep(100000);
    post(kCGEventLeftMouseUp, CGPointMake(x1, y1));
    printf("dragged\n");
    return 0;
}
