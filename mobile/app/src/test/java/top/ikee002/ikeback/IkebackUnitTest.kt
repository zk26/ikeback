package top.ikee002.ikeback

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class IkebackUnitTest {

    @Test
    fun tokenMatchesTeacherMachineReference() {
        // Same vector as ClassroomSelfTest:
        // HMAC-SHA256("ABC123", "GET\n/api/classroom/state\nnonce-0001\nSHA256(\"\")")
        assertEquals(
            "bd41d1e94c8c04a9ae857dce47c9f1d6907b65cfcd5c1bcbd8d08711a7c14e50",
            IkebackCrypto.token("ABC123", "GET", "/api/classroom/state", "nonce-0001", null),
        )
    }

    @Test
    fun tokenIsBoundToMethodPathNonceAndBody() {
        val base = { method: String, path: String, nonce: String, body: String? ->
            IkebackCrypto.token("key", method, path, nonce, body)
        }
        assertTrue(base("GET", "/a", "n", null) != base("POST", "/a", "n", null))
        assertTrue(base("GET", "/a", "n", null) != base("GET", "/b", "n", null))
        assertTrue(base("GET", "/a", "n", null) != base("GET", "/a", "n2", null))
        assertTrue(base("POST", "/a", "n", "{}") != base("POST", "/a", "n", "[]"))
        assertTrue(base("POST", "/a", "n", "{}") != base("POST", "/a", "n", null))
    }

    @Test
    fun signedResponseMatchesIndependentReferenceVector() {
        val body = "{\"ok\":true}"
        assertEquals(
            "b35ec4ba272a86a16ecc54d358f4ff721e7e1e816aabfa55c0f6f631d45483cb",
            IkebackCrypto.responseToken("ABC123", body, "nonce-0002", "1700000000"),
        )
        assertEquals(
            body,
            IkebackCrypto.verifyResponse(
                "ABC123", body, "nonce-0002", "1700000000",
                "b35ec4ba272a86a16ecc54d358f4ff721e7e1e816aabfa55c0f6f631d45483cb",
                "nonce-0002", nowMillis = 1_700_000_000_000L,
            ),
        )
    }

    @Test
    fun responseVerificationRejectsMissingTamperedStaleOrReboundData() {
        val lessonKey = "lesson-key"
        val originalBody = "{\"error\":\"denied\"}"
        val responseNonce = "response-nonce"
        val now = 1_700_000_000_000L
        val originalTimestamp = "1700000000"
        val originalSignature = IkebackCrypto.responseToken(lessonKey, originalBody, responseNonce, originalTimestamp)
        fun verify(
            body: String = originalBody,
            nonce: String? = responseNonce,
            timestamp: String? = originalTimestamp,
            signature: String? = originalSignature,
            expectedNonce: String = responseNonce,
            key: String = lessonKey,
            nowMillis: Long = now,
        ) = IkebackCrypto.verifyResponse(key, body, nonce, timestamp, signature, expectedNonce, nowMillis)

        assertThrows(ResponseIntegrityException::class.java) { verify(signature = null) }
        assertThrows(ResponseIntegrityException::class.java) { verify(signature = "00") }
        assertThrows(ResponseIntegrityException::class.java) { verify(body = "{\"error\":\"allowed\"}") }
        assertThrows(ResponseIntegrityException::class.java) { verify(key = "wrong-key") }
        assertThrows(ResponseIntegrityException::class.java) { verify(nonce = "other-nonce") }
        assertThrows(ResponseIntegrityException::class.java) { verify(expectedNonce = "other-nonce") }
        assertThrows(ResponseIntegrityException::class.java) { verify(timestamp = "1699999999", nowMillis = now + 600_000L) }
        assertThrows(ResponseIntegrityException::class.java) { verify(timestamp = "1700000601", nowMillis = now) }
        assertThrows(ResponseIntegrityException::class.java) { verify(timestamp = "not-a-time") }
    }

    @Test
    fun failedSeatSyncRetainsCacheButValidEmptyRosterReplacesIt() {
        val cached = Cache(
            className = "一班",
            students = 2,
            seatsRaw = "{\"seats\":[{\"seat\":\"1\"}]}",
            scoresRaw = "{\"old\":true}",
            updatedAt = 100L,
        )
        val state = TeacherState(teacherId = "teacher", revision = 9L, className = "")
        val scores = ScoreSnapshot(className = "一班", raw = "{\"new\":true}", lessons = listOf(mapOf("id" to "l1")))
        val failed = cached.mergeSync(state, scores, emptyList(), null, 200L)
        assertEquals(2, failed.students)
        assertEquals(cached.seatsRaw, failed.seatsRaw)
        assertEquals(scores.raw, failed.scoresRaw)
        assertEquals(9L, failed.revision)
        assertEquals(200L, failed.updatedAt)

        val empty = cached.mergeSync(state, scores, emptyList(), SeatSnapshot("{\"seats\":[]}", 0), 300L)
        assertEquals(0, empty.students)
        assertEquals("{\"seats\":[]}", empty.seatsRaw)
        assertEquals("一班", empty.className)
    }

    @Test
    fun announcementIsParsed() {
        val state = Announcement.parse("IKEBACK:11880:abc123:42:nonce9")!!
        assertEquals(11880, state.port)
        assertEquals("abc123", state.teacherId)
        assertEquals(42L, state.revision)
        assertEquals("nonce9", state.nonce)
    }

    @Test
    fun foreignPayloadIsRejected() {
        assertNull(Announcement.parse("IKEBACK_CLASSROOM:11880:abc:1:n"))
        assertNull(Announcement.parse("IKEBACK:bad:abc:1:n"))
        assertNull(Announcement.parse("IKEBACK:11880::1:n"))
    }

    @Test
    fun announcementNeverCarriesTheLessonKey() {
        assertFalse(Announcement.MAGIC.contains("key", ignoreCase = true))
        assertFalse(Announcement.PREFIX.contains("key", ignoreCase = true))
    }

    @Test
    fun singleScheduleGeneratesPeriodsAndBreaks() {
        val config = TimetableConfig(
            lessonsPerDay = 4,
            lessonMinutes = 40,
            shortBreakMinutes = 10,
            dayStart = "08:00",
            bigBreakAfter = 2,
            bigBreakMinutes = 30,
            noonAfter = 0,
            dualTime = false,
        )
        val periods = buildPeriods(config)
        // 一 08:00-08:40, 二 08:50-09:30, 大课间 09:30-10:00,
        // 三 10:00-10:40, 四 10:50-11:30
        assertEquals("一", periods[0].label)
        assertEquals(1, periods[0].times.size)
        assertEquals("08:00-08:40", periods[0].times[0])
        assertEquals("08:00", periods[0].start)
        assertEquals("08:40", periods[0].end)
        assertEquals("大课间", periods[2].label)
        assertTrue(periods[2].isBreak)
        assertEquals("09:30-10:00", periods[2].times[0])
        assertEquals("四", periods.last().label)
        assertEquals("11:30", periods.last().end)
        assertEquals(5, periods.size)
    }

    @Test
    fun dualScheduleHasTwoParallelTimeRangesPerRow() {
        val config = TimetableConfig(
            lessonsPerDay = 4,
            lessonMinutes = 40,
            shortBreakMinutes = 10,
            dayStart = "08:20",
            bigBreakAfter = 2,
            bigBreakMinutes = 30,
            noonAfter = 4,
            dualTime = true,
            timeLabel1 = "小学",
            timeLabel2 = "初高中",
            secondShiftMinutes = -20,
        )
        val periods = buildPeriods(config)
        assertEquals(2, periods[0].times.size)
        // 小学 08:20-09:00, 初高中 08:00-08:40 (shifted -20 min)
        assertEquals("08:20-09:00", periods[0].times[0])
        assertEquals("08:00-08:40", periods[0].times[1])
        assertTrue(periods[2].isBreak)
        assertEquals(2, periods[2].times.size)
        val labels = if (config.dualTime) listOf(config.timeLabel1, config.timeLabel2) else emptyList()
        assertEquals(listOf("小学", "初高中"), labels)
        assertNull(currentPeriod(Timetable(periods = periods), 7 * 60))
        assertEquals(0, currentPeriod(Timetable(periods = periods), 8 * 60 + 30))
    }

    @Test
    fun noonBreakAndColourPaletteAreTolerant() {
        // org.json is a stub in plain JVM unit tests, so only pure-Kotlin
        // helpers are asserted here.
        val config = TimetableConfig(
            lessonsPerDay = 4,
            lessonMinutes = 40,
            shortBreakMinutes = 10,
            dayStart = "08:00",
            bigBreakAfter = 0,
            noonAfter = 2,
            noonMinutes = 120,
            dualTime = false,
        )
        val periods = buildPeriods(config)
        assertEquals("午休", periods[2].label)
        assertTrue(periods[2].isBreak)
        assertEquals("09:30-11:30", periods[2].times[0])
        assertEquals("13:00", periods.last().end)
        assertFalse(config.dualTime)
        assertTrue(config.copy(dualTime = true).dualTime)
        assertEquals(4, config.lessonsPerDay)
        // Out-of-range / negative colour indices wrap instead of crashing.
        assertTrue(courseColor(-1) == courseColor(COURSE_COLORS.size - 1))
        assertTrue(courseColor(999) == courseColor(999 % COURSE_COLORS.size))
        assertNull(parseTimeRange("not-a-time"))
        assertEquals(480..540, parseTimeRange("8:00-9:00"))
    }

    @Test
    fun courseAtFindsSpanningCourses() {
        val course = TimetableCourse(
            name = "通用技术", dayOfWeek = 3, startSection = 1, endSection = 2,
            className = "高一4",
        )
        val table = Timetable(periods = buildPeriods(TimetableConfig()), courses = listOf(course))
        assertEquals(course, courseAt(table, 3, 1))
        assertEquals(course, courseAt(table, 3, 2))
        assertNull(courseAt(table, 3, 3))
        assertNull(courseAt(table, 2, 1))
    }
}
