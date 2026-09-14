package com.rvvm.android;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.widget.ScrollView;

/**
 * A ScrollView with a height ceiling: the taskbar grows with its content
 * (auto-expanded height) until it would take more than a fraction of the
 * screen - beyond that it stops growing and scrolls instead, so a full house
 * of guest windows can never push the console off the screen.
 */
public class BoundedScrollView extends ScrollView {

    /** The height ceiling, as a fraction of the screen height. */
    private static final float MAX_HEIGHT_FRACTION = 0.6f;

    public BoundedScrollView(Context context) {
        super(context);
    }

    public BoundedScrollView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int maxH = (int) (getResources().getDisplayMetrics().heightPixels * MAX_HEIGHT_FRACTION);
        super.onMeasure(widthMeasureSpec,
                View.MeasureSpec.makeMeasureSpec(maxH, View.MeasureSpec.AT_MOST));
    }
}
