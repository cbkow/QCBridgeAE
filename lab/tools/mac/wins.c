#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
int main(int argc, char** argv) {
  CFArrayRef a = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  for (CFIndex i = 0; i < CFArrayGetCount(a); ++i) {
    CFDictionaryRef d = CFArrayGetValueAtIndex(a, i);
    CFStringRef owner = CFDictionaryGetValue(d, kCGWindowOwnerName);
    CFStringRef name = CFDictionaryGetValue(d, kCGWindowName);
    CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
    CGRect r; CGRectMakeWithDictionaryRepresentation(b, &r);
    char o[256]="", n[256]="";
    if (owner) CFStringGetCString(owner, o, 256, kCFStringEncodingUTF8);
    if (name) CFStringGetCString(name, n, 256, kCFStringEncodingUTF8);
    printf("%s | %s | %.0f %.0f %.0f %.0f\n", o, n, r.origin.x, r.origin.y, r.size.width, r.size.height);
  }
  return 0;
}
