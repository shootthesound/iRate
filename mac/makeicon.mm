// makeicon.mm - renders iRate's app icon (1024px master PNG) with Core Graphics.
// Concept: a gold five-point rating star on a dusk gradient squircle, with a thin
// aperture ring and a speed streak — "fast raw rating". Build the .icns via:
//   clang++ -fobjc-arc makeicon.mm -o /tmp/makeicon -framework Cocoa && /tmp/makeicon out.png
#import <Cocoa/Cocoa.h>
#include <cmath>

static void addStar(CGContextRef c, CGFloat cx, CGFloat cy, CGFloat ro, CGFloat ri) {
    CGContextBeginPath(c);
    for (int i = 0; i < 10; i++) {
        CGFloat ang = (M_PI / 2) + i * (M_PI / 5);   // start at top, step 36°
        CGFloat r = (i % 2 == 0) ? ro : ri;
        CGFloat x = cx + cosf(ang) * r, y = cy + sinf(ang) * r;
        if (i == 0) CGContextMoveToPoint(c, x, y); else CGContextAddLineToPoint(c, x, y);
    }
    CGContextClosePath(c);
}

int main(int argc, const char** argv) {
    @autoreleasepool {
        const int S = 1024;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef c = CGBitmapContextCreate(nullptr, S, S, 8, 0, cs,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);

        // --- squircle clip (macOS icon corner radius ≈ 0.2237·size) ---
        CGRect box = CGRectMake(0, 0, S, S);
        CGPathRef rr = CGPathCreateWithRoundedRect(box, S * 0.2237, S * 0.2237, nullptr);
        CGContextSaveGState(c);
        CGContextAddPath(c, rr); CGContextClip(c);

        // --- diagonal dusk gradient: deep indigo -> crimson ---
        CGFloat comps[] = { 0.09, 0.13, 0.36, 1,   0.83, 0.12, 0.36, 1 };
        CGFloat locs[]  = { 0, 1 };
        CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, locs, 2);
        CGContextDrawLinearGradient(c, g, CGPointMake(0, S), CGPointMake(S, 0), 0);

        // --- soft top-light for depth ---
        CGFloat gl[] = { 1, 1, 1, 0.18,   1, 1, 1, 0 };
        CGGradientRef glow = CGGradientCreateWithColorComponents(cs, gl, locs, 2);
        CGContextDrawRadialGradient(c, glow, CGPointMake(S * 0.5, S * 0.82), 0,
                                    CGPointMake(S * 0.5, S * 0.82), S * 0.7, 0);

        CGFloat cx = S * 0.5, cy = S * 0.52;

        // --- speed streaks behind the star (motion = fast) ---
        CGContextSetLineCap(c, kCGLineCapRound);
        for (int i = 0; i < 3; i++) {
            CGFloat yy = cy + (i - 1) * S * 0.13;
            CGFloat w = S * (0.30 - i * 0.02);
            CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.10);
            CGContextSetLineWidth(c, S * 0.028);
            CGContextMoveToPoint(c, S * 0.12, yy);
            CGContextAddLineToPoint(c, S * 0.12 + w, yy);
            CGContextStrokePath(c);
        }

        // --- thin aperture ring (camera hint) ---
        CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.22);
        CGContextSetLineWidth(c, S * 0.012);
        CGContextAddArc(c, cx, cy, S * 0.40, 0, M_PI * 2, 0);
        CGContextStrokePath(c);

        // --- the gold rating star ---
        CGContextSetShadowWithColor(c, CGSizeMake(0, -S * 0.012), S * 0.05,
            CGColorCreateGenericRGB(0, 0, 0, 0.35));
        CGGradientRef starG = CGGradientCreateWithColorComponents(cs,
            (CGFloat[]){ 1.0, 0.86, 0.42, 1,   0.98, 0.63, 0.16, 1 }, locs, 2);
        addStar(c, cx, cy, S * 0.33, S * 0.135);
        CGContextSaveGState(c);
        CGContextClip(c);
        CGContextDrawLinearGradient(c, starG, CGPointMake(cx, cy + S * 0.33),
                                    CGPointMake(cx, cy - S * 0.33), 0);
        CGContextRestoreGState(c);
        // crisp highlight edge
        addStar(c, cx, cy, S * 0.33, S * 0.135);
        CGContextSetRGBStrokeColor(c, 1, 0.95, 0.75, 0.5);
        CGContextSetLineWidth(c, S * 0.006);
        CGContextStrokePath(c);

        CGContextRestoreGState(c);   // drop squircle clip

        // --- write PNG ---
        CGImageRef img = CGBitmapContextCreateImage(c);
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        const char* out = argc > 1 ? argv[1] : "icon_1024.png";
        [png writeToFile:[NSString stringWithUTF8String:out] atomically:YES];
        fprintf(stderr, "wrote %s\n", out);
        return 0;
    }
}
