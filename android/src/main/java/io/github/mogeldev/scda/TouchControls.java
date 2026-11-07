package io.github.mogeldev.scda;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

// On-screen controller over the game, laid out as Microsoft's touch adaptation arcs: left stick, d-pad, LT/LB on the
// left, right stick, A/B/X/Y, RT/RB on the right, Back/Start at the top. It is player 1's controller while shown
// (src/core/input.cpp, SetTouch); the native side says when to show it (AUTOMATIC: no controller plugged in).
// src/android/host.cpp registers the natives and calls attach.
public final class TouchControls extends View {
    static native void nativeSet(int buttons, int left, int right, int lx, int ly, int rx, int ry);
    static native int nativeOpacity();

    // XInput's masks; the triggers in bits XInput leaves free, sent as analog values
    private static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20;
    private static final int LS = 0x40, RS = 0x80, LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000;
    private static final int Y = 0x8000, LT = 0x10000, RT = 0x20000;
    private static final int BUTTON = 0, STICK_LEFT = 1, STICK_RIGHT = 2, DPAD = 3;

    private static final class Control {
        final int kind, mask, color;
        final String label;
        final float fx, fy, fr;  // centre as a fraction of the width/height, radius as a fraction of the height
        float x, y, r;

        Control(int kind, int mask, String label, int color, float fx, float fy, float fr) {
            this.kind = kind;
            this.mask = mask;
            this.label = label;
            this.color = color;
            this.fx = fx;
            this.fy = fy;
            this.fr = fr;
        }
    }

    private static final int WHITE = Color.WHITE;
    private final Control[] controls = {
        new Control(BUTTON, LT, "LT", WHITE, 0.05f, 0.27f, 0.065f),
        new Control(BUTTON, LB, "LB", WHITE, 0.19f, 0.27f, 0.06f),
        new Control(STICK_LEFT, 0, "", WHITE, 0.14f, 0.52f, 0.15f),
        new Control(BUTTON, LS, "LS", WHITE, 0.05f, 0.86f, 0.055f),
        new Control(DPAD, 0, "", WHITE, 0.28f, 0.77f, 0.13f),
        new Control(BUTTON, BACK, "BACK", WHITE, 0.42f, 0.09f, 0.05f),
        new Control(BUTTON, START, "START", WHITE, 0.58f, 0.09f, 0.05f),
        new Control(BUTTON, RB, "RB", WHITE, 0.81f, 0.27f, 0.06f),
        new Control(BUTTON, RT, "RT", WHITE, 0.95f, 0.27f, 0.065f),
        new Control(BUTTON, Y, "Y", 0xFFF2C744, 0.875f, 0.38f, 0.06f),
        new Control(BUTTON, X, "X", 0xFF3E8EEA, 0.82f, 0.52f, 0.06f),
        new Control(BUTTON, B, "B", 0xFFE5564B, 0.93f, 0.52f, 0.06f),
        new Control(BUTTON, A, "A", 0xFF62C454, 0.875f, 0.66f, 0.06f),
        new Control(STICK_RIGHT, 0, "", WHITE, 0.72f, 0.75f, 0.15f),
        new Control(BUTTON, RS, "RS", WHITE, 0.95f, 0.86f, 0.055f),
    };

    // a finger and the control it went down on
    private static final class Touch {
        Control control;
        float x, y;
        long down;  // uptime ms
    }

    // A quick tap is held this long: the game reads the controller once a frame and would miss a shorter press.
    private static final long MIN_PRESS_MS = 60;
    private int lingering;  // buttons of taps let go too soon, still held
    private long lingerUntil;
    private final Runnable release = () -> {
        lingering = 0;
        send();
        invalidate();
    };

    private final SparseArray<Touch> touches = new SparseArray<>();
    private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private int sentButtons, sentLx, sentLy, sentRx, sentRy, held;
    private final Runnable poll = this::poll;

    private TouchControls(Context context) {
        super(context);
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(3 * getResources().getDisplayMetrics().density);
        fill.setStyle(Paint.Style.FILL);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setVisibility(GONE);
    }

    public static void attach(Activity activity) {
        activity.runOnUiThread(() -> {
            TouchControls view = new TouchControls(activity);
            activity.addContentView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            view.poll();
        });
    }

    // shown or hidden as the settings and the plugged-in controllers say, checked four times a second
    private void poll() {
        int opacity = nativeOpacity();
        if (opacity <= 0 && getVisibility() == VISIBLE) {
            setVisibility(GONE);
            touches.clear();
            lingering = 0;
            send();
        } else if (opacity > 0) {
            setAlpha(opacity / 100f);
            setVisibility(VISIBLE);
        }
        removeCallbacks(poll);
        postDelayed(poll, 250);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        for (Control c : controls) {
            c.x = c.fx * w;
            c.y = c.fy * h;
            c.r = c.fr * h;
        }
    }

    private Control hit(float x, float y) {
        Control best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Control c : controls) {
            // the sticks take a finger a little outside their ring, buttons a quarter beyond their edge
            float reach = c.r * (c.kind == STICK_LEFT || c.kind == STICK_RIGHT ? 1.6f : 1.25f);
            float d = (float) Math.hypot(x - c.x, y - c.y);
            if (d <= reach && d / reach < bestDistance) {
                best = c;
                bestDistance = d / reach;
            }
        }
        return best;
    }

    // the d-pad's directions under a finger, diagonals included
    private static int dpad(Control c, float x, float y) {
        float dx = x - c.x;
        float dy = y - c.y;
        if (Math.hypot(dx, dy) < c.r * 0.25f) return 0;
        double angle = Math.toDegrees(Math.atan2(-dy, dx));  // 0 right, 90 up
        int mask = 0;
        if (angle > 22.5 && angle < 157.5) mask |= UP;
        if (angle < -22.5 && angle > -157.5) mask |= DOWN;
        if (angle > 112.5 || angle < -112.5) mask |= LEFT;
        if (angle < 67.5 && angle > -67.5) mask |= RIGHT;
        return mask;
    }

    // a stick's axis, -32767 to 32767, y up
    private static int axis(float value) { return Math.round(Math.max(-1f, Math.min(1f, value)) * 32767f); }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int index = e.getActionIndex();
        int id = e.getPointerId(index);
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                Control c = hit(e.getX(index), e.getY(index));
                if (c == null) break;
                Touch t = new Touch();
                t.control = c;
                t.x = e.getX(index);
                t.y = e.getY(index);
                t.down = e.getEventTime();
                touches.put(id, t);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    Touch t = touches.get(e.getPointerId(i));
                    if (t == null) continue;
                    t.x = e.getX(i);
                    t.y = e.getY(i);
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                Touch t = touches.get(id);
                touches.remove(id);
                long left = t == null ? 0 : t.down + MIN_PRESS_MS - e.getEventTime();
                if (left > 0) {
                    lingering |= buttons(t);
                    lingerUntil = Math.max(lingerUntil, e.getEventTime() + left);
                    removeCallbacks(release);
                    postDelayed(release, lingerUntil - e.getEventTime());
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                touches.clear();
                break;
            default:
                break;
        }
        send();
        invalidate();
        return true;
    }

    // the buttons a finger holds
    private static int buttons(Touch t) {
        if (t.control.kind == BUTTON) return t.control.mask;
        return t.control.kind == DPAD ? dpad(t.control, t.x, t.y) : 0;
    }

    private void send() {
        int buttons = lingering, lx = 0, ly = 0, rx = 0, ry = 0;
        for (int i = 0; i < touches.size(); i++) {
            Touch t = touches.valueAt(i);
            Control c = t.control;
            buttons |= buttons(t);
            float sx = (t.x - c.x) / c.r;
            float sy = (c.y - t.y) / c.r;
            float length = (float) Math.hypot(sx, sy);
            if (length > 1f) {
                sx /= length;
                sy /= length;
            }
            if (c.kind == STICK_LEFT) {
                lx = axis(sx);
                ly = axis(sy);
            } else if (c.kind == STICK_RIGHT) {
                rx = axis(sx);
                ry = axis(sy);
            }
        }
        // a short buzz for each button or direction that went down
        if ((buttons & ~held) != 0) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        held = buttons;
        if (buttons == sentButtons && lx == sentLx && ly == sentLy && rx == sentRx && ry == sentRy) return;
        sentButtons = buttons;
        sentLx = lx;
        sentLy = ly;
        sentRx = rx;
        sentRy = ry;
        nativeSet(buttons & 0xFFFF, (buttons & LT) != 0 ? 255 : 0, (buttons & RT) != 0 ? 255 : 0, lx, ly, rx, ry);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        for (Control c : controls) {
            boolean pressed = false;
            float knobX = c.x, knobY = c.y;
            for (int i = 0; i < touches.size(); i++) {
                Touch t = touches.valueAt(i);
                if (t.control != c) continue;
                pressed = true;
                float dx = t.x - c.x, dy = t.y - c.y;
                float length = (float) Math.hypot(dx, dy);
                float scale = length > c.r ? c.r / length : 1f;
                knobX = c.x + dx * scale;
                knobY = c.y + dy * scale;
            }
            line.setColor(c.color);
            fill.setColor(pressed ? 0x99FFFFFF : 0x55000000);
            if (c.kind == DPAD) {
                drawDpad(canvas, c);
            } else if (c.kind == BUTTON) {
                canvas.drawCircle(c.x, c.y, c.r, fill);
                canvas.drawCircle(c.x, c.y, c.r, line);
                text.setColor(c.color);
                text.setTextSize(c.r * (c.label.length() > 2 ? 0.45f : 0.8f));
                canvas.drawText(c.label, c.x, c.y - (text.ascent() + text.descent()) / 2, text);
            } else {
                fill.setColor(0x55000000);
                canvas.drawCircle(c.x, c.y, c.r, fill);
                canvas.drawCircle(c.x, c.y, c.r, line);
                fill.setColor(pressed ? 0x99FFFFFF : 0x77FFFFFF);
                canvas.drawCircle(knobX, knobY, c.r * 0.45f, fill);
            }
        }
    }

    private void drawDpad(Canvas canvas, Control c) {
        int pressed = 0;
        for (int i = 0; i < touches.size(); i++) {
            Touch t = touches.valueAt(i);
            if (t.control == c) pressed |= dpad(c, t.x, t.y);
        }
        float arm = c.r, half = c.r * 0.33f;
        int[] masks = {UP, DOWN, LEFT, RIGHT};
        for (int m : masks) {
            if (m == UP) rect.set(c.x - half, c.y - arm, c.x + half, c.y - half);
            else if (m == DOWN) rect.set(c.x - half, c.y + half, c.x + half, c.y + arm);
            else if (m == LEFT) rect.set(c.x - arm, c.y - half, c.x - half, c.y + half);
            else rect.set(c.x + half, c.y - half, c.x + arm, c.y + half);
            fill.setColor((pressed & m) != 0 ? 0x99FFFFFF : 0x55000000);
            canvas.drawRect(rect, fill);
            canvas.drawRect(rect, line);
        }
        fill.setColor(0x55000000);
        rect.set(c.x - half, c.y - half, c.x + half, c.y + half);
        canvas.drawRect(rect, fill);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(poll);
        removeCallbacks(release);
        super.onDetachedFromWindow();
    }
}
