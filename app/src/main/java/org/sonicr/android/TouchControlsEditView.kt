package org.sonicr.android

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

class TouchControlsEditView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private var bitmapA: Bitmap? = null
    private var bitmapB: Bitmap? = null

    init {
        try {
            context.assets.open("textures/Abutton.png").use { stream ->
                bitmapA = BitmapFactory.decodeStream(stream)
            }
        } catch (_: Exception) {
            try {
                context.assets.open("Abutton.png").use { stream ->
                    bitmapA = BitmapFactory.decodeStream(stream)
                }
            } catch (_: Exception) {}
        }
        try {
            context.assets.open("textures/Bbutton.png").use { stream ->
                bitmapB = BitmapFactory.decodeStream(stream)
            }
        } catch (_: Exception) {
            try {
                context.assets.open("Bbutton.png").use { stream ->
                    bitmapB = BitmapFactory.decodeStream(stream)
                }
            } catch (_: Exception) {}
        }
    }

    enum class SelectedControl {
        NONE,
        DPAD,
        DRIFT_L,
        ACCEL,
        JUMP,
        DRIFT_R,
        LOOK,
        START
    }

    var selectedControl: SelectedControl = SelectedControl.NONE
        private set

    var onSelectionChanged: ((SelectedControl, Float) -> Unit)? = null
    var onLayoutChanged: (() -> Unit)? = null

    // 7 Independent Controls (normalized [0.0, 1.0] for positions, scale factor [0.70, 1.40])
    var dpadX: Float = 0.10f
    var dpadY: Float = 0.73f
    var dpadScale: Float = 1.0f

    var driftLX: Float = 0.10f
    var driftLY: Float = 0.465f
    var driftLScale: Float = 1.0f

    var accelX: Float = 0.90f
    var accelY: Float = 0.71f
    var accelScale: Float = 1.0f

    var jumpX: Float = 0.82f
    var jumpY: Float = 0.81f
    var jumpScale: Float = 1.0f

    var driftRX: Float = 0.90f
    var driftRY: Float = 0.53f
    var driftRScale: Float = 1.0f

    var lookX: Float = 0.82f
    var lookY: Float = 0.63f
    var lookScale: Float = 1.0f

    var startX: Float = 0.90f
    var startY: Float = 0.09f
    var startScale: Float = 1.0f

    private var isCustomInitialized = false

    // Drag tracking
    private var activePointerId = MotionEvent.INVALID_POINTER_ID
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var isDragging = false

    // Paints
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val selectionPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.parseColor("#FFCC00")
        strokeWidth = 3f
        pathEffect = DashPathEffect(floatArrayOf(12f, 8f), 0f)
    }
    private val guidePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.parseColor("#3340C4FF")
        strokeWidth = 1.5f
        pathEffect = DashPathEffect(floatArrayOf(8f, 8f), 0f)
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#8040C4FF")
        textSize = 28f
        letterSpacing = 0.1f
    }

    private val tempPath = Path()
    private val tempRect = RectF()

    fun setLayoutData(
        hasCustom: Boolean,
        dpX: Float, dpY: Float, dpS: Float,
        dlX: Float, dlY: Float, dlS: Float,
        acX: Float, acY: Float, acS: Float,
        jmX: Float, jmY: Float, jmS: Float,
        drX: Float, drY: Float, drS: Float,
        lkX: Float, lkY: Float, lkS: Float,
        stX: Float, stY: Float, stS: Float
    ) {
        if (hasCustom) {
            dpadX = dpX
            dpadY = dpY
            dpadScale = dpS.coerceIn(0.70f, 1.40f)

            driftLX = dlX
            driftLY = dlY
            driftLScale = dlS.coerceIn(0.70f, 1.40f)

            accelX = acX
            accelY = acY
            accelScale = acS.coerceIn(0.70f, 1.40f)

            jumpX = jmX
            jumpY = jmY
            jumpScale = jmS.coerceIn(0.70f, 1.40f)

            driftRX = drX
            driftRY = drY
            driftRScale = drS.coerceIn(0.70f, 1.40f)

            lookX = lkX
            lookY = lkY
            lookScale = lkS.coerceIn(0.70f, 1.40f)

            startX = stX
            startY = stY
            startScale = stS.coerceIn(0.70f, 1.40f)

            isCustomInitialized = true
            clampAll()
        } else {
            resetToDefault()
        }
        invalidate()
    }

    fun resetToDefault() {
        val w = width.toFloat().takeIf { it > 0 } ?: 2400f
        val h = height.toFloat().takeIf { it > 0 } ?: 1080f

        val gameW = h * (4.0f / 3.0f)
        val margin = if (w > gameW) (w - gameW) * 0.5f else 0.0f
        var leftAnchorX = if (margin >= h * 0.25f) margin * 0.52f else h * 0.22f
        if (leftAnchorX < h * 0.18f) leftAnchorX = h * 0.18f
        val rightAnchorX = w - leftAnchorX

        // 1. D-Pad
        dpadX = leftAnchorX / w
        dpadY = 0.73f
        dpadScale = 1.0f

        // 2. Drift Left (above D-pad)
        val dpadR = h * 0.19f
        val driftLOffset = dpadR + h * 0.075f
        driftLX = leftAnchorX / w
        driftLY = (h * 0.73f - driftLOffset) / h
        driftLScale = 1.0f

        // 3. Accel (A)
        accelX = (rightAnchorX + h * 0.035f) / w
        accelY = 0.71f
        accelScale = 1.0f

        // 4. Jump (B)
        jumpX = (rightAnchorX - h * 0.145f) / w
        jumpY = 0.81f
        jumpScale = 1.0f

        // 5. Drift Right (R)
        driftRX = (rightAnchorX + h * 0.035f) / w
        driftRY = 0.53f
        driftRScale = 1.0f

        // 6. Look Back (Eye)
        lookX = (rightAnchorX - h * 0.145f) / w
        lookY = 0.63f
        lookScale = 1.0f

        // 7. Start / Pause
        startX = (w - h * 0.16f) / w
        startY = 0.09f
        startScale = 1.0f

        isCustomInitialized = true
        clampAll()
        onSelectionChanged?.invoke(selectedControl, getSelectedScale())
        onLayoutChanged?.invoke()
        invalidate()
    }

    fun setSelectedScale(scale: Float) {
        val clamped = scale.coerceIn(0.70f, 1.40f)
        when (selectedControl) {
            SelectedControl.DPAD -> dpadScale = clamped
            SelectedControl.DRIFT_L -> driftLScale = clamped
            SelectedControl.ACCEL -> accelScale = clamped
            SelectedControl.JUMP -> jumpScale = clamped
            SelectedControl.DRIFT_R -> driftRScale = clamped
            SelectedControl.LOOK -> lookScale = clamped
            SelectedControl.START -> startScale = clamped
            SelectedControl.NONE -> return
        }
        clampAll()
        onSelectionChanged?.invoke(selectedControl, clamped)
        onLayoutChanged?.invoke()
        invalidate()
    }

    fun getSelectedScale(): Float {
        return when (selectedControl) {
            SelectedControl.DPAD -> dpadScale
            SelectedControl.DRIFT_L -> driftLScale
            SelectedControl.ACCEL -> accelScale
            SelectedControl.JUMP -> jumpScale
            SelectedControl.DRIFT_R -> driftRScale
            SelectedControl.LOOK -> lookScale
            SelectedControl.START -> startScale
            SelectedControl.NONE -> 1.0f
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (!isCustomInitialized) {
            resetToDefault()
        } else {
            clampAll()
        }
    }

    private fun clampAll() {
        val w = width.toFloat().takeIf { it > 0 } ?: return
        val h = height.toFloat().takeIf { it > 0 } ?: return
        val pad = h * 0.02f

        // 1. D-Pad
        val dpadR = h * 0.19f * dpadScale
        var dpCx = dpadX * w
        var dpCy = dpadY * h
        dpCx = dpCx.coerceIn(dpadR + pad, w - dpadR - pad)
        dpCy = dpCy.coerceIn(dpadR + pad, h - dpadR - pad)
        dpadX = dpCx / w
        dpadY = dpCy / h

        // 2. Drift L
        val driftLR = h * 0.068f * driftLScale
        var dlCx = driftLX * w
        var dlCy = driftLY * h
        dlCx = dlCx.coerceIn(driftLR + pad, w - driftLR - pad)
        dlCy = dlCy.coerceIn(driftLR + pad, h - driftLR - pad)
        driftLX = dlCx / w
        driftLY = dlCy / h

        // 3. Accel (A)
        val accelR = h * 0.095f * accelScale
        var acCx = accelX * w
        var acCy = accelY * h
        acCx = acCx.coerceIn(accelR + pad, w - accelR - pad)
        acCy = acCy.coerceIn(accelR + pad, h - accelR - pad)
        accelX = acCx / w
        accelY = acCy / h

        // 4. Jump (B)
        val jumpR = h * 0.088f * jumpScale
        var jmCx = jumpX * w
        var jmCy = jumpY * h
        jmCx = jmCx.coerceIn(jumpR + pad, w - jumpR - pad)
        jmCy = jmCy.coerceIn(jumpR + pad, h - jumpR - pad)
        jumpX = jmCx / w
        jumpY = jmCy / h

        // 5. Drift R
        val driftRR = h * 0.068f * driftRScale
        var drCx = driftRX * w
        var drCy = driftRY * h
        drCx = drCx.coerceIn(driftRR + pad, w - driftRR - pad)
        drCy = drCy.coerceIn(driftRR + pad, h - driftRR - pad)
        driftRX = drCx / w
        driftRY = drCy / h

        // 6. Look Back (Eye)
        val lookR = h * 0.068f * lookScale
        var lkCx = lookX * w
        var lkCy = lookY * h
        lkCx = lkCx.coerceIn(lookR + pad, w - lookR - pad)
        lkCy = lkCy.coerceIn(lookR + pad, h - lookR - pad)
        lookX = lkCx / w
        lookY = lkCy / h

        // 7. Start / Pause
        val startW = h * 0.18f * startScale
        val startH = h * 0.075f * startScale
        val halfW = (startW + startH) * 0.5f
        val halfH = startH * 0.5f
        var stCx = startX * w
        var stCy = startY * h
        stCx = stCx.coerceIn(halfW + pad, w - halfW - pad)
        stCy = stCy.coerceIn(halfH + pad, h - halfH - pad)
        startX = stCx / w
        startY = stCy / h
    }

    private fun enforceMinDistance(control: SelectedControl, w: Float, h: Float) {
        data class CtrlInfo(val type: SelectedControl, var cx: Float, var cy: Float, val r: Float)
        val list = listOf(
            CtrlInfo(SelectedControl.DPAD, dpadX * w, dpadY * h, h * 0.19f * dpadScale),
            CtrlInfo(SelectedControl.DRIFT_L, driftLX * w, driftLY * h, h * 0.068f * driftLScale),
            CtrlInfo(SelectedControl.ACCEL, accelX * w, accelY * h, h * 0.095f * accelScale),
            CtrlInfo(SelectedControl.JUMP, jumpX * w, jumpY * h, h * 0.088f * jumpScale),
            CtrlInfo(SelectedControl.DRIFT_R, driftRX * w, driftRY * h, h * 0.068f * driftRScale),
            CtrlInfo(SelectedControl.LOOK, lookX * w, lookY * h, h * 0.068f * lookScale),
            CtrlInfo(SelectedControl.START, startX * w, startY * h, h * 0.090f * startScale)
        )

        val current = list.firstOrNull { it.type == control } ?: return
        for (other in list) {
            if (other.type == control) continue
            val dx = current.cx - other.cx
            val dy = current.cy - other.cy
            val distSq = dx * dx + dy * dy
            val minDist = 0.65f * (current.r + other.r)
            if (distSq < minDist * minDist) {
                val dist = kotlin.math.sqrt(distSq).coerceAtLeast(0.001f)
                val push = minDist - dist
                current.cx += (dx / dist) * push
                current.cy += (dy / dist) * push
            }
        }

        when (control) {
            SelectedControl.DPAD -> { dpadX = current.cx / w; dpadY = current.cy / h }
            SelectedControl.DRIFT_L -> { driftLX = current.cx / w; driftLY = current.cy / h }
            SelectedControl.ACCEL -> { accelX = current.cx / w; accelY = current.cy / h }
            SelectedControl.JUMP -> { jumpX = current.cx / w; jumpY = current.cy / h }
            SelectedControl.DRIFT_R -> { driftRX = current.cx / w; driftRY = current.cy / h }
            SelectedControl.LOOK -> { lookX = current.cx / w; lookY = current.cy / h }
            SelectedControl.START -> { startX = current.cx / w; startY = current.cy / h }
            SelectedControl.NONE -> {}
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val w = width.toFloat().takeIf { it > 0 } ?: return false
        val h = height.toFloat().takeIf { it > 0 } ?: return false

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val x = event.x
                val y = event.y

                val hit = hitTestControls(x, y, w, h)
                if (hit != SelectedControl.NONE) {
                    selectedControl = hit
                    activePointerId = event.getPointerId(0)
                    lastTouchX = x
                    lastTouchY = y
                    isDragging = true
                    onSelectionChanged?.invoke(selectedControl, getSelectedScale())
                    invalidate()
                    parent?.requestDisallowInterceptTouchEvent(true)
                    return true
                } else {
                    selectedControl = SelectedControl.NONE
                    onSelectionChanged?.invoke(selectedControl, 1.0f)
                    invalidate()
                }
            }

            MotionEvent.ACTION_MOVE -> {
                if (isDragging && activePointerId != MotionEvent.INVALID_POINTER_ID) {
                    val pointerIndex = event.findPointerIndex(activePointerId)
                    if (pointerIndex >= 0) {
                        val x = event.getX(pointerIndex)
                        val y = event.getY(pointerIndex)
                        val dx = (x - lastTouchX) / w
                        val dy = (y - lastTouchY) / h

                        when (selectedControl) {
                            SelectedControl.DPAD -> {
                                dpadX += dx
                                dpadY += dy
                            }
                            SelectedControl.DRIFT_L -> {
                                driftLX += dx
                                driftLY += dy
                            }
                            SelectedControl.ACCEL -> {
                                accelX += dx
                                accelY += dy
                            }
                            SelectedControl.JUMP -> {
                                jumpX += dx
                                jumpY += dy
                            }
                            SelectedControl.DRIFT_R -> {
                                driftRX += dx
                                driftRY += dy
                            }
                            SelectedControl.LOOK -> {
                                lookX += dx
                                lookY += dy
                            }
                            SelectedControl.START -> {
                                startX += dx
                                startY += dy
                            }
                            SelectedControl.NONE -> {}
                        }

                        enforceMinDistance(selectedControl, w, h)
                        clampAll()
                        lastTouchX = x
                        lastTouchY = y
                        onLayoutChanged?.invoke()
                        invalidate()
                        return true
                    }
                }
            }

            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                isDragging = false
                activePointerId = MotionEvent.INVALID_POINTER_ID
                parent?.requestDisallowInterceptTouchEvent(false)
            }
        }

        return super.onTouchEvent(event)
    }

    private fun hitTestControls(px: Float, py: Float, w: Float, h: Float): SelectedControl {
        // 1. Start Button test (pill)
        val stCx = startX * w
        val stCy = startY * h
        val stW = h * 0.18f * startScale
        val stH = h * 0.075f * startScale
        val stHalfW = (stW + stH) * 0.5f
        val stHalfH = stH * 0.5f
        if (px >= stCx - stHalfW - 24f && px <= stCx + stHalfW + 24f &&
            py >= stCy - stHalfH - 24f && py <= stCy + stHalfH + 24f) {
            return SelectedControl.START
        }

        // Test buttons individually (smaller/adjacent first)
        // 2. Drift L
        val dlCx = driftLX * w
        val dlCy = driftLY * h
        val dlR = h * 0.068f * driftLScale
        val distDlSq = (px - dlCx) * (px - dlCx) + (py - dlCy) * (py - dlCy)
        if (distDlSq <= (dlR * 1.4f) * (dlR * 1.4f)) {
            return SelectedControl.DRIFT_L
        }

        // 3. Drift R
        val drCx = driftRX * w
        val drCy = driftRY * h
        val drR = h * 0.068f * driftRScale
        val distDrSq = (px - drCx) * (px - drCx) + (py - drCy) * (py - drCy)
        if (distDrSq <= (drR * 1.4f) * (drR * 1.4f)) {
            return SelectedControl.DRIFT_R
        }

        // 4. Look Back (Eye)
        val lkCx = lookX * w
        val lkCy = lookY * h
        val lkR = h * 0.068f * lookScale
        val distLkSq = (px - lkCx) * (px - lkCx) + (py - lkCy) * (py - lkCy)
        if (distLkSq <= (lkR * 1.4f) * (lkR * 1.4f)) {
            return SelectedControl.LOOK
        }

        // 5. Jump (B)
        val jmCx = jumpX * w
        val jmCy = jumpY * h
        val jmR = h * 0.088f * jumpScale
        val distJmSq = (px - jmCx) * (px - jmCx) + (py - jmCy) * (py - jmCy)
        if (distJmSq <= (jmR * 1.35f) * (jmR * 1.35f)) {
            return SelectedControl.JUMP
        }

        // 6. Accel (A)
        val acCx = accelX * w
        val acCy = accelY * h
        val acR = h * 0.095f * accelScale
        val distAcSq = (px - acCx) * (px - acCx) + (py - acCy) * (py - acCy)
        if (distAcSq <= (acR * 1.35f) * (acR * 1.35f)) {
            return SelectedControl.ACCEL
        }

        // 7. D-Pad
        val dpCx = dpadX * w
        val dpCy = dpadY * h
        val dpR = h * 0.19f * dpadScale
        val distDpSq = (px - dpCx) * (px - dpCx) + (py - dpCy) * (py - dpCy)
        if (distDpSq <= (dpR * 1.30f) * (dpR * 1.30f)) {
            return SelectedControl.DPAD
        }

        return SelectedControl.NONE
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat().takeIf { it > 0 } ?: return
        val h = height.toFloat().takeIf { it > 0 } ?: return

        // 1. Draw 4:3 Game Viewport Reference Guides
        val gameW = h * (4.0f / 3.0f)
        if (w > gameW) {
            val margin = (w - gameW) * 0.5f
            canvas.drawLine(margin, 0f, margin, h, guidePaint)
            canvas.drawLine(w - margin, 0f, w - margin, h, guidePaint)
            canvas.drawText("4:3 GAME VIEWPORT", margin + 16f, h - 24f, textPaint)
        }

        // 2. Render D-Pad
        renderDpad(canvas, w, h)

        // 3. Render Drift Left
        renderDriftL(canvas, w, h)

        // 4. Render Accel (A)
        renderAccel(canvas, w, h)

        // 5. Render Jump (B)
        renderJump(canvas, w, h)

        // 6. Render Drift Right (R)
        renderDriftR(canvas, w, h)

        // 7. Render Look Back (Eye)
        renderLook(canvas, w, h)

        // 8. Render Start Button
        renderStartButton(canvas, w, h)
    }

    private fun renderDpad(canvas: Canvas, w: Float, h: Float) {
        val cx = dpadX * w
        val cy = dpadY * h
        val r = h * 0.19f * dpadScale
        val deadzone = h * 0.045f * dpadScale

        // Selection highlight ring
        if (selectedControl == SelectedControl.DPAD) {
            canvas.drawCircle(cx, cy, r + 16f, selectionPaint)
        }

        // Base circle
        fillPaint.color = Color.argb((0.45f * 255).toInt(), 15, 20, 36)
        canvas.drawCircle(cx, cy, r, fillPaint)

        // Outer ring
        strokePaint.color = Color.argb((0.40f * 255).toInt(), 115, 166, 224)
        strokePaint.strokeWidth = 3f
        canvas.drawCircle(cx, cy, r - 1.5f, strokePaint)

        // Deadzone guide ring
        strokePaint.color = Color.argb((0.25f * 255).toInt(), 90, 128, 178)
        strokePaint.strokeWidth = 2f
        canvas.drawCircle(cx, cy, deadzone, strokePaint)

        // Cardinal tick marks
        val tickInner = r * 0.76f
        val tickOuter = r * 0.90f
        strokePaint.color = Color.argb((0.40f * 255).toInt(), 204, 217, 255)
        strokePaint.strokeWidth = 2.5f
        // Up
        canvas.drawLine(cx, cy - tickInner, cx, cy - tickOuter, strokePaint)
        // Down
        canvas.drawLine(cx, cy + tickInner, cx, cy + tickOuter, strokePaint)
        // Left
        canvas.drawLine(cx - tickInner, cy, cx - tickOuter, cy, strokePaint)
        // Right
        canvas.drawLine(cx + tickInner, cy, cx + tickOuter, cy, strokePaint)

        // Draggable Thumb Knob preview (centered)
        val knobR = r * 0.38f
        // Knob body
        fillPaint.color = Color.argb((0.70f * 255).toInt(), 26, 38, 64)
        canvas.drawCircle(cx, cy, knobR, fillPaint)
        // Knob rim
        strokePaint.color = Color.argb((0.60f * 255).toInt(), 102, 166, 255)
        strokePaint.strokeWidth = 3f
        canvas.drawCircle(cx, cy, knobR - 1.5f, strokePaint)
        // Knob center accent dot
        fillPaint.color = Color.argb((0.65f * 255).toInt(), 64, 115, 173)
        canvas.drawCircle(cx, cy, knobR * 0.30f, fillPaint)
    }

    private fun renderDriftL(canvas: Canvas, w: Float, h: Float) {
        val cx = driftLX * w
        val cy = driftLY * h
        val r = h * 0.068f * driftLScale

        if (selectedControl == SelectedControl.DRIFT_L) {
            canvas.drawCircle(cx, cy, r + 14f, selectionPaint)
        }

        drawButtonCircle(
            canvas, cx, cy, r,
            Color.argb((0.50f * 255).toInt(), 217, 115, 25),
            Color.argb((0.80f * 255).toInt(), 255, 191, 77)
        )

        // "L" glyph
        strokePaint.color = Color.WHITE
        strokePaint.strokeWidth = (h * 0.007f).coerceAtLeast(3f) * driftLScale
        val s = r * 0.55f
        canvas.drawLine(cx - s * 0.30f, cy - s * 0.60f, cx - s * 0.30f, cy + s * 0.60f, strokePaint)
        canvas.drawLine(cx - s * 0.30f, cy + s * 0.60f, cx + s * 0.38f, cy + s * 0.60f, strokePaint)
    }

    private fun renderAccel(canvas: Canvas, w: Float, h: Float) {
        val cx = accelX * w
        val cy = accelY * h
        val r = h * 0.095f * accelScale

        if (selectedControl == SelectedControl.ACCEL) {
            canvas.drawCircle(cx, cy, r + 14f, selectionPaint)
        }

        val bmp = bitmapA
        if (bmp != null) {
            tempRect.set(cx - r, cy - r, cx + r, cy + r)
            canvas.drawBitmap(bmp, null, tempRect, null)
        } else {
            drawButtonCircle(
                canvas, cx, cy, r,
                Color.argb((0.50f * 255).toInt(), 26, 89, 217),
                Color.argb((0.80f * 255).toInt(), 102, 191, 255)
            )
            drawGlyphA(canvas, cx, cy, r * 0.55f, (h * 0.007f).coerceAtLeast(3f) * accelScale)
        }
    }

    private fun renderJump(canvas: Canvas, w: Float, h: Float) {
        val cx = jumpX * w
        val cy = jumpY * h
        val r = h * 0.088f * jumpScale

        if (selectedControl == SelectedControl.JUMP) {
            canvas.drawCircle(cx, cy, r + 14f, selectionPaint)
        }

        val bmp = bitmapB
        if (bmp != null) {
            tempRect.set(cx - r, cy - r, cx + r, cy + r)
            canvas.drawBitmap(bmp, null, tempRect, null)
        } else {
            drawButtonCircle(
                canvas, cx, cy, r,
                Color.argb((0.50f * 255).toInt(), 20, 166, 56),
                Color.argb((0.80f * 255).toInt(), 77, 242, 128)
            )
            drawGlyphB(canvas, cx, cy, r * 0.55f, (h * 0.007f).coerceAtLeast(3f) * jumpScale)
        }
    }

    private fun renderDriftR(canvas: Canvas, w: Float, h: Float) {
        val cx = driftRX * w
        val cy = driftRY * h
        val r = h * 0.068f * driftRScale

        if (selectedControl == SelectedControl.DRIFT_R) {
            canvas.drawCircle(cx, cy, r + 14f, selectionPaint)
        }

        drawButtonCircle(
            canvas, cx, cy, r,
            Color.argb((0.50f * 255).toInt(), 217, 115, 25),
            Color.argb((0.80f * 255).toInt(), 255, 191, 77)
        )
        drawGlyphR(canvas, cx, cy, r * 0.55f, (h * 0.007f).coerceAtLeast(3f) * driftRScale)
    }

    private fun renderLook(canvas: Canvas, w: Float, h: Float) {
        val cx = lookX * w
        val cy = lookY * h
        val r = h * 0.068f * lookScale

        if (selectedControl == SelectedControl.LOOK) {
            canvas.drawCircle(cx, cy, r + 14f, selectionPaint)
        }

        drawButtonCircle(
            canvas, cx, cy, r,
            Color.argb((0.50f * 255).toInt(), 153, 51, 204),
            Color.argb((0.80f * 255).toInt(), 217, 140, 255)
        )
        drawGlyphEye(canvas, cx, cy, r * 0.55f, (h * 0.007f).coerceAtLeast(3f) * lookScale)
    }

    private fun renderStartButton(canvas: Canvas, w: Float, h: Float) {
        val cx = startX * w
        val cy = startY * h
        val startW = h * 0.18f * startScale
        val startH = h * 0.075f * startScale
        val halfW = startW * 0.5f
        val halfH = startH * 0.5f

        // Selection highlight
        if (selectedControl == SelectedControl.START) {
            tempRect.set(cx - halfW - halfH - 12f, cy - halfH - 12f, cx + halfW + halfH + 12f, cy + halfH + 12f)
            canvas.drawRoundRect(tempRect, halfH + 12f, halfH + 12f, selectionPaint)
        }

        // Pill Body
        tempRect.set(cx - halfW - halfH, cy - halfH, cx + halfW + halfH, cy + halfH)
        fillPaint.color = Color.argb((0.50f * 255).toInt(), 191, 38, 38)
        canvas.drawRoundRect(tempRect, halfH, halfH, fillPaint)

        strokePaint.color = Color.argb((0.80f * 255).toInt(), 255, 102, 102)
        strokePaint.strokeWidth = 2.5f * startScale
        canvas.drawRoundRect(tempRect, halfH, halfH, strokePaint)

        // Glyph (play triangle + 2 pause bars)
        val s = halfH * 0.85f
        val thick = (h * 0.007f).coerceAtLeast(3f) * startScale

        val triX = cx - s * 0.32f
        tempPath.reset()
        tempPath.moveTo(triX + s * 0.32f, cy)
        tempPath.lineTo(triX - s * 0.32f, cy - s * 0.40f)
        tempPath.lineTo(triX - s * 0.32f, cy + s * 0.40f)
        tempPath.close()
        fillPaint.color = Color.WHITE
        canvas.drawPath(tempPath, fillPaint)

        val barX = cx + s * 0.22f
        canvas.drawRect(barX, cy - s * 0.40f, barX + thick * 1.4f, cy + s * 0.40f, fillPaint)
        canvas.drawRect(barX + thick * 2.4f, cy - s * 0.40f, barX + thick * 3.8f, cy + s * 0.40f, fillPaint)
    }

    private fun drawButtonCircle(canvas: Canvas, cx: Float, cy: Float, r: Float, fillColor: Int, strokeColor: Int) {
        fillPaint.color = fillColor
        canvas.drawCircle(cx, cy, r, fillPaint)
        strokePaint.color = strokeColor
        strokePaint.strokeWidth = 3f
        canvas.drawCircle(cx, cy, r - 1.5f, strokePaint)
    }

    private fun drawGlyphA(canvas: Canvas, cx: Float, cy: Float, s: Float, thick: Float) {
        strokePaint.color = Color.WHITE
        strokePaint.strokeWidth = thick
        canvas.drawLine(cx - s * 0.45f, cy + s * 0.65f, cx, cy - s * 0.65f, strokePaint)
        canvas.drawLine(cx, cy - s * 0.65f, cx + s * 0.45f, cy + s * 0.65f, strokePaint)
        canvas.drawLine(cx - s * 0.26f, cy + s * 0.15f, cx + s * 0.26f, cy + s * 0.15f, strokePaint)
    }

    private fun drawGlyphB(canvas: Canvas, cx: Float, cy: Float, s: Float, thick: Float) {
        strokePaint.color = Color.WHITE
        strokePaint.strokeWidth = thick
        val left = cx - s * 0.38f
        val right = cx + s * 0.32f
        val top = cy - s * 0.65f
        val mid = cy
        val bot = cy + s * 0.65f

        canvas.drawLine(left, top, left, bot, strokePaint)
        canvas.drawLine(left, top, right - s * 0.12f, top, strokePaint)
        canvas.drawLine(right - s * 0.12f, top, right, top + (mid - top) * 0.5f, strokePaint)
        canvas.drawLine(right, top + (mid - top) * 0.5f, right - s * 0.12f, mid, strokePaint)
        canvas.drawLine(right - s * 0.12f, mid, left, mid, strokePaint)

        canvas.drawLine(right - s * 0.12f, mid, right + s * 0.05f, mid + (bot - mid) * 0.5f, strokePaint)
        canvas.drawLine(right + s * 0.05f, mid + (bot - mid) * 0.5f, right - s * 0.12f, bot, strokePaint)
        canvas.drawLine(right - s * 0.12f, bot, left, bot, strokePaint)
    }

    private fun drawGlyphR(canvas: Canvas, cx: Float, cy: Float, s: Float, thick: Float) {
        strokePaint.color = Color.WHITE
        strokePaint.strokeWidth = thick
        val left = cx - s * 0.38f
        val right = cx + s * 0.35f
        val top = cy - s * 0.60f
        val mid = cy - s * 0.05f
        val bot = cy + s * 0.60f

        canvas.drawLine(left, top, left, bot, strokePaint)
        canvas.drawLine(left, top, right - s * 0.10f, top, strokePaint)
        canvas.drawLine(right - s * 0.10f, top, right, top + (mid - top) * 0.5f, strokePaint)
        canvas.drawLine(right, top + (mid - top) * 0.5f, right - s * 0.10f, mid, strokePaint)
        canvas.drawLine(right - s * 0.10f, mid, left, mid, strokePaint)
        canvas.drawLine(left + s * 0.08f, mid, right, bot, strokePaint)
    }

    private fun drawGlyphEye(canvas: Canvas, cx: Float, cy: Float, s: Float, thick: Float) {
        strokePaint.color = Color.WHITE
        strokePaint.strokeWidth = thick
        canvas.drawLine(cx - s * 0.65f, cy, cx, cy - s * 0.38f, strokePaint)
        canvas.drawLine(cx, cy - s * 0.38f, cx + s * 0.65f, cy, strokePaint)
        canvas.drawLine(cx + s * 0.65f, cy, cx, cy + s * 0.38f, strokePaint)
        canvas.drawLine(cx, cy + s * 0.38f, cx - s * 0.65f, cy, strokePaint)
        fillPaint.color = Color.WHITE
        canvas.drawCircle(cx, cy, s * 0.20f, fillPaint)
    }
}
