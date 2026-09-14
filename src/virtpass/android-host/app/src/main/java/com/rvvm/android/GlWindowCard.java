package com.rvvm.android;

import android.app.Activity;
import android.graphics.drawable.Drawable;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * One guest window: the floating card a run's graphics output lives in.
 *
 * Each run gets its own card - its own SurfaceView, its own surface, its own
 * geometry - instead of the single card every run used to share. The window
 * metaphor is unchanged from the single-card days: a caption bar that drags,
 * the Windows three-button set (minimize to the taskbar, maximize/restore,
 * close ends the run), content-driven visibility (the card waits at 1x1 px
 * until the guest's first frame, because the surface has to exist from the
 * start but must not take room on screen before there is something to show),
 * and the video area letterboxing the panel's aspect ratio.
 *
 * Everything window-shaped is per-card state here; the host (MainActivity)
 * owns what crosses runs: the workspace, the taskbar strip, the run table and
 * the active-guest pointer. The card talks to the host through the Host
 * interface - surface lifecycle, touches on the video area (forwarded to the
 * guest), and the close button (ends that card's run).
 */
public class GlWindowCard {

    /** The window's host: the Activity that owns the run table. */
    public interface Host {
        /** The card's surface is up (surfaceCreated). */
        void onCardSurfaceCreated(GlWindowCard card, SurfaceHolder holder);
        /** The card's surface changed size (surfaceChanged). */
        void onCardSurfaceChanged(GlWindowCard card, SurfaceHolder holder,
                                  int format, int width, int height);
        /** The card's surface is going away (surfaceDestroyed). */
        void onCardSurfaceDestroyed(GlWindowCard card, SurfaceHolder holder);
        /** The framework wants the card's surface redrawn (surfaceRedrawNeeded). */
        void onCardRedrawNeeded(GlWindowCard card, SurfaceHolder holder);
        /** A touch on the card's video area: forward it to this card's guest. */
        void onCardTouch(GlWindowCard card, MotionEvent event);
        /** The close button: end this card's run. */
        void onCardClosed(GlWindowCard card);
        /** The card was tapped/dragged by the user: make its run the
         *  foreground one (console, keyboard input and the run controls act
         *  on the foreground guest). */
        void onCardFocused(GlWindowCard card);
        /** The soft keyboard may be up; drop it (maximize takes the workspace). */
        void onCardMaximized();
        /** The card was minimized to the taskbar (rebuild the strip). */
        void onCardMinimized(GlWindowCard card);
        /** The card came back from the taskbar chip (rebuild the strip). */
        void onCardRestored(GlWindowCard card);
    }

    private static final String TAG = "RVVM-GlWindowCard";

    private final Host host;
    private final int guestId;

    // Views (all resolved through the card's own view: ids repeat across cards).
    public final View cardView;
    public final SurfaceView surfaceView;
    private final FitFrameLayout videoArea;
    private final View captionBar;
    private final TextView caption;
    private final TextView btnMin, btnMax, btnClose;
    private final ViewGroup workspace;   // the container the card lives in

    // Decorations as inflated from gl_window.xml, put back on the first frame.
    private final int framePadding;
    private final float floatElevation;
    private final Drawable background;

    // Floating geometry as inflated: the maximized state is entered and left by
    // re-laying the card out, so the way back has to be written down first.
    private final int floatW, floatH, floatGravity;
    private float restoreLeft, restoreTop;

    private boolean revealed = false;
    private boolean minimized = false;
    private boolean maximized = false;
    private boolean surfaceReady = false;
    private String title = "";

    // The focused window's title bar is tinted (the Windows active-caption
    // cue); unfocused ones stay the plain dark grey from the layout.
    private static final int CAPTION_FOCUSED = 0xFF1F5FA8;
    private static final int CAPTION_UNFOCUSED = 0xFF303030;

    /** Create the card for a run and add it to the workspace, waiting at 1x1. */
    public GlWindowCard(Activity activity, ViewGroup workspace, int guestId, String guestName, Host host) {
        this.host = host;
        this.guestId = guestId;
        this.workspace = workspace;
        this.title = guestName != null ? guestName : "";

        cardView = activity.getLayoutInflater().inflate(R.layout.gl_window, workspace, false);
        surfaceView = cardView.findViewById(R.id.surfaceView);
        videoArea = cardView.findViewById(R.id.glVideoArea);
        captionBar = cardView.findViewById(R.id.glCaptionBar);
        caption = cardView.findViewById(R.id.glCaption);
        btnMin = cardView.findViewById(R.id.glBtnMin);
        btnMax = cardView.findViewById(R.id.glBtnMax);
        btnClose = cardView.findViewById(R.id.glBtnClose);

        // The picture is fitted to the panel's ratio, never stretched: a
        // SurfaceView can only be scaled by the rectangle it is given, so the
        // ratio goes on the video area. Set before the first layout, so the
        // surface is created at its final size.
        videoArea.setAspectRatio((float) MainActivity.PANEL_W / MainActivity.PANEL_H);

        // Has to be set before the surface is created to take effect: the card
        // composites above the activity's window, so it can float over the console.
        surfaceView.setZOrderMediaOverlay(true);

        framePadding = cardView.getPaddingLeft();
        floatElevation = cardView.getElevation();
        background = cardView.getBackground();
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) cardView.getLayoutParams();
        floatW = lp.width;
        floatH = lp.height;
        floatGravity = lp.gravity;

        caption.setText(title.isEmpty() ? activity.getString(R.string.gl_title) : title);

        // Nothing to show yet: the card waits for the guest's first frame.
        hide();
        applyLayout();
        workspace.addView(cardView);

        btnMin.setOnClickListener(v -> minimize());
        btnMax.setOnClickListener(v -> toggleMaximized());
        btnClose.setOnClickListener(v -> host.onCardClosed(this));

        bindDrag();

        // Touches are consumed by the WHOLE card, not just the SurfaceView:
        // the letterboxed video leaves black bars around the frame (a
        // maximized card on a tall phone has huge ones), and a tap there used
        // to fall through to whatever view sat below - focusing another
        // window or toggling the console. The host maps the coordinates into
        // the surface's own space (see onCardTouch).
        cardView.setOnTouchListener((v, event) -> {
            host.onCardTouch(this, event);
            return true;
        });

        // The framework right before the surface is shown again (and after
        // every create/change): let the guest repaint immediately.
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback2() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                surfaceReady = true;
                host.onCardSurfaceCreated(GlWindowCard.this, holder);
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                surfaceReady = true;
                host.onCardSurfaceChanged(GlWindowCard.this, holder, format, width, height);
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceReady = false;
                host.onCardSurfaceDestroyed(GlWindowCard.this, holder);
            }
            @Override public void surfaceRedrawNeeded(SurfaceHolder holder) {
                host.onCardRedrawNeeded(GlWindowCard.this, holder);
            }
        });
    }

    public int getGuestId() { return guestId; }

    public String getTitle() { return title; }

    public boolean isSurfaceReady() { return surfaceReady; }

    public boolean isMinimized() { return minimized; }

    /** Focus cue: the active window's title bar is tinted, the rest stay grey. */
    public void setFocused(boolean focused) {
        captionBar.setBackgroundColor(focused ? CAPTION_FOCUSED : CAPTION_UNFOCUSED);
    }

    /* ============================================================
     * Geometry
     * ============================================================ */

    /** Lay the card out for its current state: waiting for a frame, maximized
     *  (fills the workspace) or floating (its own size). The position is a
     *  margin, not a translation - see moveWindow() for why. */
    private void applyLayout() {
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) cardView.getLayoutParams();
        if (!revealed) {
            // Waiting for a frame: the surface gets the smallest box that keeps
            // it alive, parked where the window will grow from.
            lp.width = 1;
            lp.height = 1;
            lp.gravity = floatGravity;
            cardView.setLayoutParams(lp);
            return;
        }
        if (maximized) {
            lp.width = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.height = FrameLayout.LayoutParams.MATCH_PARENT;
            lp.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
            lp.leftMargin = lp.topMargin = lp.rightMargin = lp.bottomMargin = 0;
        } else {
            lp.width = floatW;
            lp.height = floatH;
            lp.gravity = floatGravity;
        }
        cardView.setLayoutParams(lp);
    }

    /**
     * Take the card's footprint away without taking its surface away.
     *
     * Every part of this matters, because a SurfaceView only has a surface
     * while its content area is non-zero: at 1x1 px with the padding removed
     * and the title bar gone, the surface stays alive at the smallest size the
     * framework grants - and a zero-sized SurfaceView has no surface, which is
     * exactly the state that strands a guest waiting for INIT_WINDOW.
     */
    private void hide() {
        captionBar.setVisibility(View.GONE);
        cardView.setPadding(0, 0, 0, 0);
        cardView.setBackground(null);
        cardView.setElevation(0f);
    }

    /**
     * The guest has a frame for the window (or the user asked for it back):
     * put the card's frame, title bar, shadow and size back.
     *
     * Nothing is recreated under the guest: the card only grows over the
     * surface that has been there all along - no new surface, no INIT_WINDOW,
     * and at most the resize the guest would see if the card had been dragged
     * to that size by hand.
     */
    private void reveal() {
        if (revealed) {
            return;
        }
        revealed = true;
        cardView.setBackground(background);
        cardView.setElevation(floatElevation);
        cardView.setPadding(framePadding, framePadding, framePadding, framePadding);
        captionBar.setVisibility(View.VISIBLE);
        applyLayout();
    }

    /**
     * Put the card's top-left corner at (left, top) in workspace coordinates,
     * clamped so it always stays inside the workspace.
     *
     * The move is a re-layout (a margin), never a translation. A SurfaceView
     * composites its own surface and follows the *layout* position, so a
     * translated card would slide the caption bar out from under the video it
     * is supposed to be a window around. Positioning by margin keeps the two
     * together, at the price of a layout pass per motion event - which this
     * hierarchy (a card with two children) is not going to notice.
     *
     * The card is laid out with horizontal gravity END, so the margin that
     * places it on the X axis is the right one: right = width - w - left.
     */
    private void moveWindow(float left, float top) {
        if (maximized) {
            return;
        }
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) cardView.getLayoutParams();
        // Explicit sizes are what the floating state uses; the card still
        // reports the maximized one until the next layout runs.
        float w = lp.width > 0 ? lp.width : cardView.getWidth();
        float h = lp.height > 0 ? lp.height : cardView.getHeight();
        float maxLeft = Math.max(0f, workspace.getWidth() - w);
        float maxTop = Math.max(0f, workspace.getHeight() - h);
        left = clamp(left, 0f, maxLeft);
        top = clamp(top, 0f, maxTop);
        lp.rightMargin = (int) Math.max(0f, workspace.getWidth() - w - left);
        lp.topMargin = (int) top;
        cardView.setLayoutParams(lp);
    }

    /** Re-apply the current position against a workspace that may have changed
     *  size (soft keyboard, rotation). Only the clamps can move the card. */
    public void clampToWorkspace() {
        if (maximized) {
            return;
        }
        moveWindow(cardView.getLeft(), cardView.getTop());
    }

    private static float clamp(float value, float lo, float hi) {
        return value < lo ? lo : (value > hi ? hi : value);
    }

    /**
     * Windows-style cascade: step this card down-right by {@code index}
     * diagonal steps from its floating corner, so simultaneously-running
     * guests do not stack pixel-for-pixel. When a step would leave the
     * workspace the walk reflects off the edge (ping-pong) instead of
     * wrapping past it - the same bounce a dragged window meets in
     * moveWindow(), applied to the spawn point.
     *
     * Posted until the workspace is laid out: a card is created before its
     * container has a size to cascade within.
     */
    public void cascadeTo(final int index) {
        cardView.post(() -> {
            if (workspace.getWidth() <= 0 || workspace.getHeight() <= 0) {
                // The workspace is not laid out yet (the card was created in
                // the same pass that first measures it): a cascade computed
                // now would clamp to (0,0). Try again on the next pass.
                cascadeTo(index);
                return;
            }
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) cardView.getLayoutParams();
            float w = lp.width > 0 ? lp.width : cardView.getWidth();
            float h = lp.height > 0 ? lp.height : cardView.getHeight();
            float margin = dp(12);
            float step = dp(28);
            float spanX = Math.max(0f, workspace.getWidth() - w - 2f * margin);
            float spanY = Math.max(0f, workspace.getHeight() - h - 2f * margin);
            int stepsX = Math.max(1, (int) (spanX / step));
            int stepsY = Math.max(1, (int) (spanY / step));
            float left = margin + triangle(index, stepsX) * step;
            float top = margin + triangle(index, stepsY) * step;
            moveWindow(left, top);
        });
    }

    /** Ping-pong walk: 0,1..steps,steps-1..1,0,1.. - the reflection at both
     *  edges. steps >= 1. */
    private static int triangle(int index, int steps) {
        int period = 2 * steps;
        int m = index % period;
        return m <= steps ? m : period - m;
    }

    private float dp(int v) {
        return v * cardView.getResources().getDisplayMetrics().density;
    }

    /* ============================================================
     * Window states
     * ============================================================ */

    /** Maximize button: fill the workspace with the guest's window, or put it
     *  back at the corner and size it was floating at. Fullscreen drops the
     *  1px border ring (the frame drawable only shows through the padding) -
     *  a maximized window is edge-to-edge, like a desktop maximized one. The
     *  button flips between the maximize and the restore glyph, so the state
     *  is readable off the caption bar. */
    private void toggleMaximized() {
        if (maximized) {
            maximized = false;
            // Border back: the 1dp frame ring around the surface.
            cardView.setBackground(background);
            cardView.setPadding(framePadding, framePadding, framePadding, framePadding);
            applyLayout();
            // Not restored from the margins, which the maximized layout
            // cleared: the position is re-derived from the corner it had, and
            // clamped again on the way back.
            moveWindow(restoreLeft, restoreTop);
        } else {
            restoreLeft = cardView.getLeft();
            restoreTop = cardView.getTop();
            maximized = true;
            cardView.setBackground(null);
            cardView.setPadding(0, 0, 0, 0);
            applyLayout();
            // The console the card just covered is also where the keyboard
            // would have gone.
            host.onCardMaximized();
        }
        btnMax.setText(maximized ? R.string.gl_btn_restore : R.string.gl_btn_max);
    }

    /** Minimize button: take the window off the workspace and leave it in the
     *  taskbar. The guest is not touched; the surface going away gives the
     *  guest its TERM_WINDOW. */
    private void minimize() {
        minimized = true;
        cardView.setVisibility(View.GONE);
        host.onCardMinimized(this);
    }

    /** The chip: bring the minimized window back. Revealing covers a window
     *  minimized before it ever had a frame (nothing was shown, so the chip
     *  would otherwise hand back the 1x1 waiting state). */
    public void restore() {
        minimized = false;
        cardView.setVisibility(View.VISIBLE);
        reveal();
        host.onCardRestored(this);
    }

    /** The first presented frame is on the surface: grow the card over it.
     *  No-op for a window the user already minimized (the chip restores it). */
    public void revealOnFirstFrame() {
        if (!minimized) {
            reveal();
        }
    }

    /** The run is over (or the user closed it): take the card down for good.
     *  A card that never got its first frame is left alone - it is invisible
     *  anyway, and hiding it would only delay the framework's surface teardown
     *  the next run would have to wait for. */
    public void dismiss() {
        workspace.removeView(cardView);
    }

    private void bindDrag() {
        final int slop = ViewConfiguration.get(cardView.getContext()).getScaledTouchSlop();
        caption.setOnTouchListener(new View.OnTouchListener() {
            private float fromX, fromY, baseLeft, baseTop;
            private boolean dragging;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        // Grabbing a window focuses it, the same as a desktop.
                        host.onCardFocused(GlWindowCard.this);
                        // Raw coordinates: the view moves under the finger.
                        fromX = event.getRawX();
                        fromY = event.getRawY();
                        baseLeft = cardView.getLeft();
                        baseTop = cardView.getTop();
                        dragging = false;
                        return true;
                    case MotionEvent.ACTION_MOVE: {
                        if (maximized) {
                            return true;    // fills the workspace; nowhere to go
                        }
                        float dx = event.getRawX() - fromX;
                        float dy = event.getRawY() - fromY;
                        if (!dragging) {
                            if (Math.hypot(dx, dy) < slop) {
                                return true;    // still a tap candidate
                            }
                            dragging = true;
                        }
                        moveWindow(baseLeft + dx, baseTop + dy);
                        return true;
                    }
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        dragging = false;
                        return true;
                    default:
                        return false;
                }
            }
        });
    }
}
