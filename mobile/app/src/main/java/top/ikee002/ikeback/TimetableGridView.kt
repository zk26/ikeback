package top.ikee002.ikeback

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat
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

private fun Context.themeColor(attribute: Int, fallback: Int): Int {
    val values = obtainStyledAttributes(intArrayOf(attribute))
    return try {
        values.getColor(0, fallback)
    } finally {
        values.recycle()
    }
}

/** Resolve theme-aware colors once per view, never hardcode a light canvas. */
private class GridColors(context: Context) {
    val textPrimary = context.themeColor(android.R.attr.textColorPrimary, context.getColor(R.color.ikeback_title))
    val textSecondary = context.themeColor(android.R.attr.textColorSecondary, context.getColor(R.color.ikeback_secondary))
    val textMuted = context.getColor(R.color.ikeback_hint)
    val breakText = context.getColor(R.color.ikeback_grid_break_text)
    val headerBg = context.getColor(R.color.ikeback_grid_header)
    val breakBg = context.getColor(R.color.ikeback_grid_break)
    val line = context.getColor(R.color.ikeback_grid_line)
    val todayAxisBg = context.getColor(R.color.ikeback_grid_today_axis)
    val todayGridBg = context.getColor(R.color.ikeback_grid_today)
    val todayChip = context.getColor(R.color.ikeback_primary)
    val todayRowBg = context.getColor(R.color.ikeback_grid_today_row)
    val onPrimary = context.getColor(R.color.ikeback_on_primary)
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
        set(value) {
            field = value
            contentDescription = "作息时间。" + value.periods.joinToString("；") { period ->
                "${period.label} ${period.times.joinToString("、")}"
            }
            requestLayout()
            invalidate()
        }
    var nowRow: Int? = null
        set(value) { field = value; invalidate() }

    private val colors = GridColors(context)
    private val density get() = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    // Cached paints: a getter here would allocate a Paint on every access,
    // i.e. dozens of times per onDraw frame.
    init {
        contentDescription = "作息时间。" + timetable.periods.joinToString("；") { period ->
            "${period.label} ${period.times.joinToString("、")}"
        }
    }
    private val labelPaint by lazy { textPaint(12f, colors.textPrimary, true) }
    private val timePaint by lazy { textPaint(9f, colors.textSecondary, false) }
    private val breakPaint by lazy { textPaint(11f, colors.textSecondary, true) }
    private val todayFill = colors.todayAxisBg

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

        fill.color = colors.headerBg
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
                fill.color = colors.breakBg
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
            fill.color = colors.line
            canvas.drawRect(0f, bottom - d, w, bottom, fill)
        }
    }
}

/** Scrollable day grid: today column, break rows, coloured spanning course blocks. */
class TimetableCourseGridView(context: Context) : View(context) {
    var timetable: Timetable = Timetable()
        set(value) {
            field = value
            ensureSelectedCell()
            contentDescription = accessibilitySummary()
            requestLayout()
            invalidate()
        }
    var nowRow: Int? = null
        set(value) { field = value; invalidate() }
    var onCellClick: ((day: Int, row: Int) -> Unit)? = null
        set(value) {
            field = value
            isClickable = value != null
            contentDescription = accessibilitySummary()
        }

    private var selectedCell: Pair<Int, Int>? = null
    private val colors = GridColors(context)
    private val density get() = resources.displayMetrics.density
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    // Cached paints (see TimetableAxisView): avoid per-frame allocations.
    private val dayPaint by lazy { textPaint(11f, colors.textPrimary, true) }
    private val datePaint by lazy { textPaint(9f, colors.textMuted, false) }
    private val todayTextPaint by lazy { textPaint(11f, colors.onPrimary, true) }
    private val blockText by lazy { textPaint(11f, Color.WHITE, true) }
    private val blockSub by lazy { textPaint(9f, Color.WHITE, false) }
    private val breakPaint by lazy { textPaint(11f, colors.breakText, true) }

    init {
        // Custom canvases need explicit focus/click semantics for TalkBack and keyboards.
        isClickable = true
        isFocusable = true
        ensureSelectedCell()
        contentDescription = accessibilitySummary()
        ViewCompat.replaceAccessibilityAction(
            this,
            AccessibilityNodeInfoCompat.AccessibilityActionCompat(ACTION_PREVIOUS_CELL, "上一个课格"),
            "上一个课格",
        ) { _, _ ->
            moveSelectedCell(-1)
            true
        }
        ViewCompat.replaceAccessibilityAction(
            this,
            AccessibilityNodeInfoCompat.AccessibilityActionCompat(ACTION_NEXT_CELL, "下一个课格"),
            "下一个课格",
        ) { _, _ ->
            moveSelectedCell(1)
            true
        }
        ViewCompat.replaceAccessibilityAction(
            this,
            AccessibilityNodeInfoCompat.AccessibilityActionCompat(
                AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_CLICK.id,
                "编辑当前课格",
            ),
            "编辑当前课格",
        ) { _, _ ->
            performClick()
            true
        }
    }

    private fun moveSelectedCell(offset: Int) {
        val cells = timetable.periods.indices
            .filter { !timetable.periods[it].isBreak }
            .flatMap { row -> (1..7).map { day -> day to row } }
        val current = selectedCell?.let { cells.indexOf(it) } ?: -1
        val target = (current + offset).takeIf { it in cells.indices } ?: return
        selectCell(cells[target].first, cells[target].second, announce = true)
    }

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
        fill.color = colors.todayGridBg
        canvas.drawRect(dayW * (today - 1), 0f, dayW * today, height.toFloat(), fill)

        // Header: weekday + date, today gets a filled rounded chip.
        for (i in 0..6) {
            val left = dayW * i
            if (i + 1 == today) {
                fill.color = colors.todayChip
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
                fill.color = colors.todayRowBg
                canvas.drawRect(0f, top, width.toFloat(), bottom, fill)
            }
            if (period.isBreak) {
                fill.color = colors.breakBg
                canvas.drawRect(0f, top, width.toFloat(), bottom, fill)
                canvas.drawText(period.label, width / 2f, (top + bottom) / 2f + 4f * d, breakPaint)
            }
            fill.color = colors.line
            canvas.drawRect(0f, bottom - d, width.toFloat(), bottom, fill)
        }
        for (i in 1..6) {
            fill.color = colors.line
            canvas.drawRect(dayW * i - d, 0f, dayW * i, height.toFloat(), fill)
        }

        // A restrained focus outline makes keyboard/TalkBack cell selection visible.
        if (isFocused) selectedCell?.let { (day, row) ->
            val left = dayW * (day - 1) + 2f * d
            val right = dayW * day - 2f * d
            val box = RectF(left, tops[row] + 2f * d, right, tops[row + 1] - 2f * d)
            fill.color = colors.textPrimary
            fill.style = Paint.Style.STROKE
            fill.strokeWidth = 2f * d
            canvas.drawRoundRect(box, 8f * d, 8f * d, fill)
            fill.style = Paint.Style.FILL
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

    private fun ensureSelectedCell() {
        if (selectedCell?.let { (day, row) ->
                day in 1..7 && timetable.periods.getOrNull(row)?.isBreak == false
            } == true
        ) return
        val row = nowRow?.takeIf { timetable.periods.getOrNull(it)?.isBreak == false }
            ?: timetable.periods.indices.firstOrNull { !timetable.periods[it].isBreak }
        selectedCell = row?.let { todayDay() to it }
    }

    private fun cellDescription(day: Int, row: Int): String {
        val period = timetable.periods.getOrNull(row)
        val course = courseAt(timetable, day, row)
        val detail = course?.let {
            listOf(it.name, it.className, it.room, it.teacher).filter { value -> value.isNotBlank() }
                .joinToString("，")
        } ?: "无课"
        return "${WEEKDAYS.getOrElse(day - 1) { "" }} ${period?.label.orEmpty()}，$detail"
    }

    private fun accessibilitySummary(): String = buildString {
        append("本周课表。方向键选择课格，双击或按确认键编辑。")
        for (day in 1..7) {
            val lessons = timetable.periods.indices
                .filter { !timetable.periods[it].isBreak }
                .mapNotNull { row -> courseAt(timetable, day, row)?.let { "${timetable.periods[row].label} ${it.name}" } }
                .distinct()
            append(WEEKDAYS.getOrElse(day - 1) { "" }).append("：")
            append(lessons.ifEmpty { listOf("无课") }.joinToString("；"))
            append("。")
        }
        selectedCell?.let { (day, row) -> append("当前课格：").append(cellDescription(day, row)).append("。") }
    }

    private fun selectCell(day: Int, row: Int, announce: Boolean = false) {
        if (day !in 1..7 || timetable.periods.getOrNull(row)?.isBreak != false) return
        selectedCell = day to row
        contentDescription = accessibilitySummary()
        invalidate()
        val tops = rowTops(timetable, density)
        val dayWidth = GridMetrics.DAY_COL_W * density
        requestRectangleOnScreen(Rect(
            (dayWidth * (day - 1)).toInt(),
            tops[row].toInt(),
            (dayWidth * day).toInt(),
            tops[row + 1].toInt(),
        ), false)
        if (announce) announceForAccessibility(cellDescription(day, row))
    }

    override fun onFocusChanged(gainFocus: Boolean, direction: Int, previouslyFocusedRect: android.graphics.Rect?) {
        super.onFocusChanged(gainFocus, direction, previouslyFocusedRect)
        if (gainFocus) {
            ensureSelectedCell()
            selectedCell?.let { (day, row) -> selectCell(day, row, announce = true) }
        }
    }

    override fun performClick(): Boolean {
        val handled = super.performClick()
        val (day, row) = selectedCell ?: return handled
        onCellClick?.invoke(day, row)
        sendAccessibilityEvent(android.view.accessibility.AccessibilityEvent.TYPE_VIEW_CLICKED)
        return true
    }

    override fun onKeyDown(keyCode: Int, event: android.view.KeyEvent?): Boolean {
        ensureSelectedCell()
        val (day, row) = selectedCell ?: return super.onKeyDown(keyCode, event)
        var targetDay = day
        var targetRow = row
        when (keyCode) {
            android.view.KeyEvent.KEYCODE_DPAD_LEFT -> targetDay--
            android.view.KeyEvent.KEYCODE_DPAD_RIGHT -> targetDay++
            android.view.KeyEvent.KEYCODE_DPAD_UP -> targetRow--
            android.view.KeyEvent.KEYCODE_DPAD_DOWN -> targetRow++
            android.view.KeyEvent.KEYCODE_ENTER, android.view.KeyEvent.KEYCODE_NUMPAD_ENTER -> {
                performClick()
                return true
            }
            else -> return super.onKeyDown(keyCode, event)
        }
        // Skip break rows when moving vertically, while keeping horizontal movement simple.
        if (keyCode == android.view.KeyEvent.KEYCODE_DPAD_UP || keyCode == android.view.KeyEvent.KEYCODE_DPAD_DOWN) {
            val step = if (keyCode == android.view.KeyEvent.KEYCODE_DPAD_UP) -1 else 1
            targetRow = row + step
            while (targetRow in timetable.periods.indices && timetable.periods[targetRow].isBreak) targetRow += step
        }
        if (targetDay !in 1..7 || timetable.periods.getOrNull(targetRow)?.isBreak != false) return true
        selectCell(targetDay, targetRow, announce = true)
        return true
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.action == MotionEvent.ACTION_DOWN) {
            val d = density
            val day = (event.x / (GridMetrics.DAY_COL_W * d)).toInt() + 1
            val tops = rowTops(timetable, d)
            val row = (0 until timetable.periods.size).firstOrNull {
                event.y >= tops[it] && event.y < tops[it + 1]
            }
            if (day in 1..7 && row != null) selectCell(day, row)
            else {
                selectedCell = null
                contentDescription = accessibilitySummary()
                invalidate()
            }
            return true
        }
        if (event.action == MotionEvent.ACTION_UP) {
            performClick()
            return true
        }
        return super.onTouchEvent(event)
    }

    private companion object {
        const val ACTION_PREVIOUS_CELL = 0x100001
        const val ACTION_NEXT_CELL = 0x100002
    }
}
