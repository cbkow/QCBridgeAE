#include <ApplicationServices/ApplicationServices.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
static void post(CGEventType t, CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, kCGMouseButtonLeft);
    CGEventPost(kCGHIDEventTap, e); CFRelease(e);
}
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "click x y [double]\n"); return 2; }
    CGPoint p = CGPointMake(atof(argv[1]), atof(argv[2]));
    post(kCGEventMouseMoved, p); usleep(120000);
    post(kCGEventLeftMouseDown, p); usleep(70000); post(kCGEventLeftMouseUp, p);
    if (argc > 3) {
        usleep(90000);
        CGEventRef d = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, p, kCGMouseButtonLeft);
        CGEventSetIntegerValueField(d, kCGMouseEventClickState, 2); CGEventPost(kCGHIDEventTap, d); CFRelease(d);
        usleep(70000);
        CGEventRef u = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, p, kCGMouseButtonLeft);
        CGEventSetIntegerValueField(u, kCGMouseEventClickState, 2); CGEventPost(kCGHIDEventTap, u); CFRelease(u);
    }
    printf("clicked %.0f %.0f\n", p.x, p.y);
    return 0;
}
