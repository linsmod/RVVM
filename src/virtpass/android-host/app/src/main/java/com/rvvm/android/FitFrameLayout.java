package com.rvvm.android;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.widget.FrameLayout;

/**
 * Container that lays its child out at a fixed width:height ratio: as large as
 * fits in the box, centred, with the rest of the box left to whatever is drawn
 * behind it - the black bars of a letterbox.
 *
 * The graphics window needs it because of how a SurfaceView presents: its
 * surface is a layer the compositor scales into the view's rectangle, so the
 * rectangle is the *only* lever on the picture's aspect. Stretching a 1280x720
 * guest frame into a card of another shape (a maximized window on a portrait
 * phone, say) distorts it, and no amount of host-side scaling can fix that
 * afterwards - the frame has already been drawn. Sizing the rectangle instead
 * keeps the guest's own pixels square, and the bars around it are just this
 * container showing through.
 *
 * The child is measured at its exact size rather than scaled by a transform, so
 * nothing has to be clipped: a SurfaceView ignores the view hierarchy's clipping
 * anyway.
 */
public class FitFrameLayout extends FrameLayout {

    /** Width:height the child is laid out at. Zero or less means "fill". */
    private float aspect = 0f;

    public FitFrameLayout(Context context) {
        this(context, null);
    }

    public FitFrameLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    /**
     * Set the ratio the child is fitted to. Called before the first layout, so
     * the first traversal already measures the child at its final size.
     */
    public void setAspectRatio(float ratio) {
        if (ratio > 0f && ratio != aspect) {
            aspect = ratio;
            requestLayout();
        }
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        final int width  = MeasureSpec.getSize(widthMeasureSpec);
        final int height = MeasureSpec.getSize(heightMeasureSpec);
        setMeasuredDimension(width, height);

        int childWidth = width;
        int childHeight = height;
        if (aspect > 0f && width > 0 && height > 0) {
            if ((float) width / height > aspect) {
                // Wider than the content: bars left and right.
                childHeight = height;
                childWidth = Math.round(height * aspect);
            } else {
                // Taller than the content: bars above and below.
                childWidth = width;
                childHeight = Math.round(width / aspect);
            }
        }
        /* Never zero: a SurfaceView is only given a surface while its box is
         * non-empty, and the window is deliberately laid out 1x1 px while it
         * waits for the guest's first frame - a box of nothing there would take
         * the guest's window away with it. */
        childWidth = Math.max(1, childWidth);
        childHeight = Math.max(1, childHeight);

        final int exactW = MeasureSpec.makeMeasureSpec(childWidth, MeasureSpec.EXACTLY);
        final int exactH = MeasureSpec.makeMeasureSpec(childHeight, MeasureSpec.EXACTLY);
        for (int i = 0; i < getChildCount(); i++) {
            final View child = getChildAt(i);
            if (child.getVisibility() != GONE) {
                // FrameLayout.onLayout() centres it with the same gravity the
                // XML asks for, so the child only needs its size from here.
                child.measure(exactW, exactH);
            }
        }
    }
}
