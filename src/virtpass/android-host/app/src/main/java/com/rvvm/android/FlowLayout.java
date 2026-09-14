package com.rvvm.android;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;

/**
 * Flow layout: children are laid out left-to-right, wrapping to the next line
 * when the current one runs out of width - the taskbar chip grid needs to
 * grow DOWN (auto-expanded height) instead of scrolling sideways, so every
 * running window stays visible at a glance.
 */
public class FlowLayout extends ViewGroup {

    private final int lineSpacing;
    private final int itemSpacing;

    public FlowLayout(Context context) {
        this(context, null);
    }

    public FlowLayout(Context context, AttributeSet attrs) {
        super(context, attrs);
        float density = context.getResources().getDisplayMetrics().density;
        lineSpacing = (int) (4 * density);
        itemSpacing = (int) (4 * density);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int maxW = MeasureSpec.getSize(widthMeasureSpec);
        int x = getPaddingLeft();
        int y = getPaddingTop();
        int rowHeight = 0;
        int rightEdge = 0;

        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            measureChildWithMargins(child, widthMeasureSpec, 0, heightMeasureSpec, 0);
            MarginLayoutParams lp = (MarginLayoutParams) child.getLayoutParams();
            int cw = child.getMeasuredWidth() + lp.leftMargin + lp.rightMargin;
            int ch = child.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;

            if (x > getPaddingLeft() && x + cw > maxW - getPaddingRight()) {
                x = getPaddingLeft();
                y += rowHeight + lineSpacing;
                rowHeight = 0;
            }
            x += cw + itemSpacing;
            rightEdge = Math.max(rightEdge, x);
            rowHeight = Math.max(rowHeight, ch);
        }

        int totalH = y + rowHeight + getPaddingBottom();
        int totalW = rightEdge + getPaddingRight();
        setMeasuredDimension(resolveSize(totalW, widthMeasureSpec),
                resolveSize(totalH, heightMeasureSpec));
    }

    @Override
    protected LayoutParams generateDefaultLayoutParams() {
        // Margin-aware defaults: the wrap logic below reads margins.
        return new MarginLayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT);
    }

    @Override
    public LayoutParams generateLayoutParams(AttributeSet attrs) {
        return new MarginLayoutParams(getContext(), attrs);
    }

    @Override
    protected LayoutParams generateLayoutParams(LayoutParams p) {
        return new MarginLayoutParams(p);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int maxW = r - l;
        int x = getPaddingLeft();
        int y = getPaddingTop();
        int rowHeight = 0;

        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            MarginLayoutParams lp = (MarginLayoutParams) child.getLayoutParams();
            int cw = child.getMeasuredWidth() + lp.leftMargin + lp.rightMargin;
            int ch = child.getMeasuredHeight() + lp.topMargin + lp.bottomMargin;

            if (x > getPaddingLeft() && x + cw > maxW - getPaddingRight()) {
                x = getPaddingLeft();
                y += rowHeight + lineSpacing;
                rowHeight = 0;
            }
            int left = x + lp.leftMargin;
            int top = y + lp.topMargin;
            child.layout(left, top, left + child.getMeasuredWidth(),
                    top + child.getMeasuredHeight());
            x += cw + itemSpacing;
            rowHeight = Math.max(rowHeight, ch);
        }
    }
}
