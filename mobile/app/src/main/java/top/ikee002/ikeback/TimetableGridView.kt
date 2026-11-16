package top.ikee002.ikeback

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import java.util.Calendar

/**
 * ClassBoard-style weekly timetable, re-implemented with plain Android Views.
 *
 * Layout is split in two so the time axis stays frozen while the seven day
 * columns scroll horizontally. Both views compute their height with the same
 * [rowHeights] algorithm, so they stay aligned inside a common vertical
 * ScrollView.
 *
 * Reference (concepts only, no code copied):
 *   G:\AndWork\ClassBoard\app\src\main\java\top\ikee002\classboard\ui\schedule\ScheduleGridArea.kt
 */
private object GridMetrics {
    const val TIME_COL_W = 66
    const val TIME_SUBCOL_W = 56
    const val DAY_COL_W = 62
    const val CLASS_ROW_H = 68
    const val BREAK_ROW_H = 40
    const val HEADER_H = 46

    fun timeColWidth(t: Timetable): Int =
        if (t.timeLabels.size > 1) TIME_SUBCOL_W * 2 else TIME_COL_W

    fun rowHeight(p: TimetablePeriod): Int = if (p.isBreak) BREAK_ROW_H else CLASS_ROW_H
}

private fun View.dp(value: Int): Float = value * resources.displayMetrics.density

/** Parsed once: Color.parseColor per onDraw frame causes needless allocations. */
private object GridColors {
    val textPrimary = Color.parseColor("#1F2D3D")
    val textSecondary = Color.parseColor("#5A6B7B")
    val textMuted = Color.parseColor("#8A97A6")
    val breakText = Color.parseColor("#7A7392")
    val headerBg = Color.parseColor("#F7F9FC")
    val breakBg = Color.parseColor("#EDEFF5")
    val line = Color.parseColor("#E8ECF2")
    val todayAxisBg = Color.parseColor("#F2EEFF")
    val todayGridBg = Color.parseColor("#F4F0FF")
    val todayChip = Color.parseColor("#7C5CFF")
    val todayRowBg = Color.parseColor("#EDE8FF")
}

private fun weekDates(): List<Pair<String, Int>> {
    val c = Calendar.getInstance()
    c.set(Calendar.DAY_OF_WEEK, Calendar.MONDAY)
    return (0..6).map { i ->
        val d = Calendar.getInstance().apply {
            timeInMillis = c.timeInMillis
            add(Calendar.DAY_OF_MONTH, i)
        }
        "${d.get(Calendar.MONTH) + 1}/${d.get(Calendar.DAY_OF_MONTH)}" to d.get(Calendar.DAY_OF_MONTH)
    }
}

private fun rowTops(t: Timetable, density: Float): List<Float> {
    val tops = mutableListOf<Float>()
    var acc = GridMetrics.HEADER_H * density
    t.periods.forEach { p ->
        tops.add(acc)
        acc += GridMetrics.rowHeight(p) * density
    }
    tops.add(acc)
    return tops
}

private fun totalHeight(t: Timetable, density: Float): Int {
    var h = GridMetrics.HEADER_H
    t.periods.forEach { h += GridMetrics.rowHeight(it) }
    return (h * density).toInt()
}

private fun View.textPaint(sizeSp: Float, color: Int, bold: Boolean): Paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
    this.color = color
    textSize = android.util.TypedValue.applyDimension(
        android.util.TypedValue.COMPLEX_UNIT_SP, sizeSp, resources.displayMetrics
    )
    textAlign = Paint.Align.CENTER
    if (bold) typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
}

/** Frozen left axis: period label + one or two parallel time ranges. */
class TimetableAxisView(context: Context) : View(context) {
    var timetable: Timetable = Timetable()
        set(value) { field = value; requestLayout(); invalidate() }
    var nowRow: Int? = null
        set(value) { field = value; invalidate() }

    private val density get() = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    // Cached paints: a getter here would allocate a Paint on every access,
    // i.e. dozens of times per onDraw frame.
    private val labelPaint by lazy { textPaint(12f, GridColors.textPrimary, true) }
    private val timePaint by lazy { textPaint(9f, GridColors.textSecondary, false) }
    private val breakPaint by lazy { textPaint(11f, GridColors.textSecondary, true) }
    private val todayFill = GridColors.todayAxisBg

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(
            (GridMetrics.timeColWidth(timetable) * density).toInt(),
            totalHeight(timetable, density),
        )
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val d = density
        val cols = if (timetable.timeLabels.size > 1) 2 else 1
        val colW = w / cols
        val headerH = GridMetrics.HEADER_H * d

        fill.color = GridColors.headerBg
        canvas.drawRect(0f, 0f, w, headerH, fill)
        if (cols == 2) {
            for (i in 0..1) {
                val label = timetable.timeLabels.getOrNull(i) ?: ""
                canvas.drawText(label, colW * (i + 0.5f), headerH * 0.62f, labelPaint)
            }
        } else {
            canvas.drawText("时间", w / 2f, headerH * 0.62f, labelPaint)
        }

        val tops = rowTops(timetable, d)
        timetable.periods.forEachIndexed { index, period ->
            val top = tops[index]
            val bottom = tops[index + 1]
            if (index == nowRow && !period.isBreak) {
                fill.color = todayFill
                canvas.drawRect(0f, top, w, bottom, fill)
            }
            if (period.isBreak) {
                fill.color = GridColors.breakBg
                canvas.drawRect(0f, top, w, bottom, fill)
                canvas.drawText(period.label, w / 2f, (top + bottom) / 2f + 4f * d, breakPaint)
            } else {
                canvas.drawText(period.label, w / 2f, top + 20f * d, labelPaint)
                if (cols == 2) {
                    for (i in 0..1) {
                        val t = period.times.getOrNull(i) ?: ""
                        canvas.drawText(t, colW * (i + 0.5f), top + 48f * d, timePaint)
                    }
                } else {
                    canvas.drawText(period.times.firstOrNull() ?: "", w / 2f, top + 48f * d, timePaint)
                }
            }
            fill.color = GridColors.line
            canvas.drawRect(0f, bottom - d, w, bottom, fill)
        }
    }
}

/** Scrollable day grid: today column, break rows, coloured spanning course blocks. */
class TimetableCourseGridView(context: Context) : View(context) {
    init {
        // Without this, ACTION_DOWN returns false and ACTION_UP is never
        // delivered, so tapping a cell would silently do nothing.
        isClickable = true
        isFocusable = false
    }

    var timetable: Timetable = Timetable()
        set(value) { field = value; requestLayout(); invalidate() }
    var nowRow: Int? = null
        set(value) { field = value; invalidate() }
    var onCellClick: ((day: Int, row: Int) -> Unit)? = null

    private val density get() = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    // Cached paints (see TimetableAxisView): avoid per-frame allocations.
    private val dayPaint by lazy { textPaint(11f, GridColors.textPrimary, true) }
    private val datePaint by lazy { textPaint(9f, GridColors.textMuted, false) }
    private val todayTextPaint by lazy { textPaint(11f, Color.WHITE, true) }
    private val blockText by lazy { textPaint(11f, Color.WHITE, true) }
    private val blockSub by lazy { textPaint(9f, Color.WHITE, false) }
    private val breakPaint by lazy { textPaint(11f, GridColors.breakText, true) }

    // weekDates() builds 8 Calendar instances; cache it and refresh once a day.
    private var cachedDates: List<Pair<String, Int>>? = null
    private var cachedDatesDay = -1
    private fun dates(): List<Pair<String, Int>> {
        val today = Calendar.getInstance().get(Calendar.DAY_OF_YEAR)
        val cached = cachedDates
        if (cached == null || cachedDatesDay != today) {
            cachedDates = weekDates()
            cachedDatesDay = today
        }
        return cachedDates!!
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        setMeasuredDimension(
            (GridMetrics.DAY_COL_W * 7 * density).toInt(),
            totalHeight(timetable, density),
        )
    }

    override fun onDraw(canvas: Canvas) {
        val d = density
        val dayW = GridMetrics.DAY_COL_W * d
        val headerH = GridMetrics.HEADER_H * d
        val tops = rowTops(timetable, d)
        val today = todayDay()
        val dates = dates()

        // Today column background.
        fill.color = GridColors.todayGridBg
        canvas.drawRect(dayW * (today - 1), 0f, dayW * today, height.toFloat(), fill)

        // Header: weekday + date, today gets a filled rounded chip.
        for (i in 0..6) {
            val left = dayW * i
            if (i + 1 == today) {
                fill.color = GridColors.todayChip
                val chip = RectF(left + 5f * d, 6f * d, left + dayW - 5f * d, headerH - 6f * d)
                canvas.drawRoundRect(chip, 12f * d, 12f * d, fill)
                canvas.drawText(WEEKDAYS[i], left + dayW / 2f, headerH * 0.42f, todayTextPaint)
                canvas.drawText("${dates[i].second}", left + dayW / 2f, headerH * 0.76f, todayTextPaint)
            } else {
                canvas.drawText(WEEKDAYS[i], left + dayW / 2f, headerH * 0.45f, dayPaint)
                canvas.drawText(dates[i].first, left + dayW / 2f, headerH * 0.78f, datePaint)
            }
        }

        // Rows: current-period highlight, break rows spanning the whole week.
        timetable.periods.forEachIndexed { index, period ->
            val top = tops[index]
            val bottom = tops[index + 1]
            if (index == nowRow && !period.isBreak) {
                fill.color = GridColors.todayRowBg
                canvas.drawRect(0f, top, width.toFloat(), bottom, fill)
            }
            if (period.isBreak) {
                fill.color = GridColors.breakBg
                canvas.drawRect(0f, top, width.toFloat(), bottom, fill)
                canvas.drawText(period.label, width / 2f, (top + bottom) / 2f + 4f * d, breakPaint)
            }
            fill.color = GridColors.line
            canvas.drawRect(0f, bottom - d, width.toFloat(), bottom, fill)
        }
        for (i in 1..6) {
            fill.color = GridColors.line
            canvas.drawRect(dayW * i - d, 0f, dayW * i, height.toFloat(), fill)
        }

        // Course blocks, positioned by rowTops so a block can span several rows.
        val drawn = mutableSetOf<Int>()
        timetable.courses.forEachIndexed { index, course ->
            if (index in drawn) return@forEachIndexed
            val start = course.startSection.coerceAtLeast(0)
            val end = course.endSection.coerceAtMost(tops.size - 2)
            if (start > end || start >= tops.size - 1 || course.dayOfWeek !in 1..7) return@forEachIndexed
            // Skip a course anchored on a break row (dirty imported data).
            if (timetable.periods.getOrNull(start)?.isBreak == true) return@forEachIndexed
            val left = dayW * (course.dayOfWeek - 1) + 3f * d
            val right = dayW * course.dayOfWeek - 3f * d
            val top = tops[start] + 3f * d
            val bottom = tops[end + 1] - 3f * d
            if (bottom <= top) return@forEachIndexed
            val box = RectF(left, top, right, bottom)
            fill.color = courseColor(course.colorIndex)
            canvas.drawRoundRect(box, 10f * d, 10f * d, fill)
            if (start == nowRow) {
                fill.color = Color.WHITE
                fill.style = Paint.Style.STROKE
                fill.strokeWidth = 2f * d
                canvas.drawRoundRect(box, 10f * d, 10f * d, fill)
                fill.style = Paint.Style.FILL
            }
            val cx = (left + right) / 2f
            canvas.drawText(
                course.name.take(8), cx, top + 18f * d, blockText
            )
            val sub = listOf(course.className, course.room, course.teacher)
                .firstOrNull { it.isNotBlank() } ?: ""
            if (sub.isNotBlank() && bottom - top > 34f * d) {
                canvas.drawText(sub.take(9), cx, top + 34f * d, blockSub)
            }
            drawn.add(index)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN) return true
        if (event.action == MotionEvent.ACTION_UP) {
            val d = density
            val dayW = GridMetrics.DAY_COL_W * d
            val day = (event.x / dayW).toInt() + 1
            val tops = rowTops(timetable, d)
            if (day in 1..7 && event.y >= GridMetrics.HEADER_H * d) {
                val row = (0 until timetable.periods.size).firstOrNull { i ->
                    event.y >= tops[i] && event.y < tops[i + 1]
                }
                if (row != null && timetable.periods[row].isBreak.not()) {
                    onCellClick?.invoke(day, row)
                    return true
                }
            }
        }
        return super.onTouchEvent(event)
    }
}
