
package com.android.systemui;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Paint.Align;
import android.graphics.Paint.Style;
import android.graphics.Path;
import android.graphics.Path.Direction;
import android.graphics.Path.Op;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import com.android.systemui.statusbar.policy.BatteryController.BatteryStateChangeCallback;

public class ModsBatteryMeterDrawable extends Drawable implements BatteryStateChangeCallback {
    public static final String TAG = BatteryMeterDrawable.class.getSimpleName();
    private final Paint mBatteryPaint;
    private final RectF mBoltFrame = new RectF();
    private final Paint mBoltPaint;
    private final Path mBoltPath = new Path();
    private final float[] mBoltPoints;
    private int mChargeColor;
    private boolean mCharging;
    private final Path mClipPath = new Path();
    private final int[] mColors;
    private final Context mContext;
    private final int mCriticalLevel;
    private int mDarkModeBackgroundColor;
    private int mDarkModeFillColor;
    private final RectF mFrame = new RectF();
    private final Paint mFramePaint;
    private final Handler mHandler;
    private int mHeight;
    private int mIconTint = -1;
    private final int mIntrinsicHeight;
    private final int mIntrinsicWidth;
    private int mLevel = -1;
    private int mLightModeBackgroundColor;
    private int mLightModeFillColor;
    private final Path mModPath = new Path();
    private float mOldDarkIntensity = 0.0f;
    private final Path mShapePath = new Path();
    private float mSubpixelSmoothingLeft;
    private float mSubpixelSmoothingRight;
    private final Paint mTextPaint;
    private String mWarningString;
    private float mWarningTextHeight;
    private final Paint mWarningTextPaint;
    private int mWidth;

    public ModsBatteryMeterDrawable(Context context, Handler handler, int frameColor) {
        this.mContext = context;
        this.mHandler = handler;
        Resources res = context.getResources();
        TypedArray levels = res.obtainTypedArray(R.array.batterymeter_color_levels);
        TypedArray colors = res.obtainTypedArray(R.array.batterymeter_color_values);
        int N = levels.length();
        this.mColors = new int[(N * 2)];
        for (int i = 0; i < N; i++) {
            this.mColors[i * 2] = levels.getInt(i, 0);
            this.mColors[(i * 2) + 1] = colors.getColor(i, 0);
        }
        levels.recycle();
        colors.recycle();
        this.mWarningString = context.getString(R.string.battery_meter_very_low_overlay_symbol);
        this.mCriticalLevel = this.mContext.getResources().getInteger(17694804);
        this.mSubpixelSmoothingLeft = context.getResources().getFraction(R.fraction.battery_subpixel_smoothing_left, 1, 1);
        this.mSubpixelSmoothingRight = context.getResources().getFraction(R.fraction.battery_subpixel_smoothing_right, 1, 1);
        this.mFramePaint = new Paint(1);
        this.mFramePaint.setColor(frameColor);
        this.mFramePaint.setDither(true);
        this.mFramePaint.setStrokeWidth(0.0f);
        this.mFramePaint.setStyle(Style.FILL_AND_STROKE);
        this.mBatteryPaint = new Paint(1);
        this.mBatteryPaint.setDither(true);
        this.mBatteryPaint.setStrokeWidth(0.0f);
        this.mBatteryPaint.setStyle(Style.FILL_AND_STROKE);
        this.mTextPaint = new Paint(1);
        this.mTextPaint.setTypeface(Typeface.create("sans-serif-condensed", 1));
        this.mTextPaint.setTextAlign(Align.CENTER);
        this.mWarningTextPaint = new Paint(1);
        this.mWarningTextPaint.setColor(this.mColors[1]);
        this.mWarningTextPaint.setTypeface(Typeface.create("sans-serif", 1));
        this.mWarningTextPaint.setTextAlign(Align.CENTER);
        this.mChargeColor = context.getColor(R.color.batterymeter_charge_color);
        this.mBoltPaint = new Paint(1);
        this.mBoltPaint.setColor(context.getColor(R.color.batterymeter_bolt_color));
        this.mBoltPoints = loadBoltPoints(res);
        this.mDarkModeBackgroundColor = context.getColor(R.color.dark_mode_icon_color_dual_tone_background);
        this.mDarkModeFillColor = context.getColor(R.color.dark_mode_icon_color_dual_tone_fill);
        this.mLightModeBackgroundColor = context.getColor(R.color.light_mode_icon_color_dual_tone_background);
        this.mLightModeFillColor = context.getColor(R.color.light_mode_icon_color_dual_tone_fill);
        this.mIntrinsicWidth = context.getResources().getDimensionPixelSize(R.dimen.battery_width);
        this.mIntrinsicHeight = context.getResources().getDimensionPixelSize(R.dimen.battery_height);
    }

    public int getIntrinsicHeight() {
        return this.mIntrinsicHeight;
    }

    public int getIntrinsicWidth() {
        return this.mIntrinsicWidth;
    }

    private void postInvalidate() {
        this.mHandler.post(new Runnable() {
            public void run() {
                ModsBatteryMeterDrawable.this.invalidateSelf();
            }
        });
    }

    public void onBatteryLevelChanged(int level, boolean pluggedIn, boolean charging) {
        this.mLevel = level;
        this.mCharging = charging;
        Intent batteryIntent = this.mContext.registerReceiver(null, new IntentFilter("android.intent.action.BATTERY_CHANGED"));
        postInvalidate();
    }

    public void onPowerSaveChanged(boolean isPowerSave) {
        invalidateSelf();
    }

    private static float[] loadBoltPoints(Resources res) {
        int i;
        int[] pts = res.getIntArray(R.array.batterymeter_bolt_points);
        int maxX = 0;
        int maxY = 0;
        for (i = 0; i < pts.length; i += 2) {
            maxX = Math.max(maxX, pts[i]);
            maxY = Math.max(maxY, pts[i + 1]);
        }
        float[] ptsF = new float[pts.length];
        for (i = 0; i < pts.length; i += 2) {
            ptsF[i] = ((float) pts[i]) / ((float) maxX);
            ptsF[i + 1] = ((float) pts[i + 1]) / ((float) maxY);
        }
        return ptsF;
    }

    public void setBounds(int left, int top, int right, int bottom) {
        super.setBounds(left, top, right, bottom);
        this.mHeight = bottom - top;
        this.mWidth = right - left;
        this.mWarningTextPaint.setTextSize(((float) this.mHeight) * 0.7f);
        this.mWarningTextHeight = -this.mWarningTextPaint.getFontMetrics().ascent;
    }

    private int getColorForLevel(int percent) {
        int color = 0;
        int i = 0;
        while (i < this.mColors.length) {
            int thresh = this.mColors[i];
            color = this.mColors[i + 1];
            if (percent > thresh) {
                i += 2;
            } else if (i == this.mColors.length - 2) {
                return this.mIconTint;
            } else {
                return color;
            }
        }
        return color;
    }

    public void draw(Canvas c) {
        int level = this.mLevel;
        if (level != -1) {
            float levelTop;
            float drawFrac = ((float) level) / 100.0f;
            int width = (int) (((float) this.mHeight) * 0.6551724f);
            int px = (this.mWidth - width) / 2;
            this.mFrame.set(0.0f, 0.0f, (float) width, (float) this.mHeight);
            this.mFrame.offset((float) px, 0.0f);
            this.mBatteryPaint.setColor(this.mCharging ? this.mChargeColor : getColorForLevel(level));
            RectF rectF = this.mFrame;
            rectF.left += this.mSubpixelSmoothingLeft;
            rectF = this.mFrame;
            rectF.top += this.mSubpixelSmoothingLeft;
            rectF = this.mFrame;
            rectF.right -= this.mSubpixelSmoothingRight;
            rectF = this.mFrame;
            rectF.bottom -= this.mSubpixelSmoothingRight;
            if (level >= 96) {
                drawFrac = 1.0f;
            } else if (level <= this.mCriticalLevel) {
                drawFrac = 0.0f;
            }
            if (drawFrac == 1.0f) {
                levelTop = this.mFrame.top;
            } else {
                levelTop = this.mFrame.top + (this.mFrame.height() * (1.0f - drawFrac));
            }
            this.mShapePath.reset();
            this.mShapePath.addRoundRect(this.mFrame.left, this.mFrame.top, this.mFrame.right, this.mFrame.bottom, 3.0f, 3.0f, Direction.CW);
            float circleR = (this.mFrame.right - this.mFrame.left) * 0.175f;
            float circleY = this.mFrame.top + (1.5f * circleR);
            float circleX = this.mFrame.left + ((this.mFrame.right - this.mFrame.left) / 2.0f);
            this.mModPath.reset();
            this.mModPath.addCircle(circleX, circleY, circleR, Direction.CW);
            this.mShapePath.op(this.mModPath, Op.DIFFERENCE);
            if (this.mCharging) {
                float bbAosp = this.mFrame.bottom - (this.mFrame.height() / 10.0f);
                float bt = this.mFrame.top + (circleY * 2.0f);
                float bb = bbAosp;
                float aospToModScalingRatio = (bbAosp - (this.mFrame.top + (this.mFrame.height() / 6.0f))) / (bbAosp - bt);
                float bl = this.mFrame.left + ((this.mFrame.width() * aospToModScalingRatio) / 4.0f);
                float br = this.mFrame.right - ((this.mFrame.width() * aospToModScalingRatio) / 4.0f);
                if (!(this.mBoltFrame.left == bl && this.mBoltFrame.top == bt && this.mBoltFrame.right == br && this.mBoltFrame.bottom == bbAosp)) {
                    this.mBoltFrame.set(bl, bt, br, bbAosp);
                    this.mBoltPath.reset();
                    this.mBoltPath.moveTo(this.mBoltFrame.left + (this.mBoltPoints[0] * this.mBoltFrame.width()), this.mBoltFrame.top + (this.mBoltPoints[1] * this.mBoltFrame.height()));
                    for (int i = 2; i < this.mBoltPoints.length; i += 2) {
                        this.mBoltPath.lineTo(this.mBoltFrame.left + (this.mBoltPoints[i] * this.mBoltFrame.width()), this.mBoltFrame.top + (this.mBoltPoints[i + 1] * this.mBoltFrame.height()));
                    }
                    this.mBoltPath.lineTo(this.mBoltFrame.left + (this.mBoltPoints[0] * this.mBoltFrame.width()), this.mBoltFrame.top + (this.mBoltPoints[1] * this.mBoltFrame.height()));
                }
                if (Math.min(Math.max((this.mBoltFrame.bottom - levelTop) / (this.mBoltFrame.bottom - this.mBoltFrame.top), 0.0f), 1.0f) <= 0.3f) {
                    c.drawPath(this.mBoltPath, this.mBoltPaint);
                } else {
                    this.mShapePath.op(this.mBoltPath, Op.DIFFERENCE);
                }
            }
            c.drawPath(this.mShapePath, this.mFramePaint);
            this.mFrame.top = levelTop;
            this.mClipPath.reset();
            this.mClipPath.addRect(this.mFrame, Direction.CCW);
            this.mShapePath.op(this.mClipPath, Op.INTERSECT);
            c.drawPath(this.mShapePath, this.mBatteryPaint);
            if (!this.mCharging && level <= this.mCriticalLevel) {
                c.drawText(this.mWarningString, ((float) this.mWidth) * 0.5f, (((float) this.mHeight) + this.mWarningTextHeight) * 0.52f, this.mWarningTextPaint);
            }
        }
    }

    public void setAlpha(int alpha) {
    }

    public void setColorFilter(ColorFilter colorFilter) {
    }

    public int getOpacity() {
        return 0;
    }
}