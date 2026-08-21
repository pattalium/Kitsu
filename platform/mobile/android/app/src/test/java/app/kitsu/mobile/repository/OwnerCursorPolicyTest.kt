package app.kitsu.mobile.repository

import app.kitsu.mobile.transport.ConnectionMode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class OwnerCursorPolicyTest {
    private val direct = OwnerCursorNamespace(ConnectionMode.DIRECT_BLE, "KTDEAD")

    @Test fun firstLiveReadNeverUsesDisplayCacheCursor() {
        assertNull(OwnerCursorPolicy.resume("backend:opaque", null, direct))
        assertTrue(OwnerCursorPolicy.shouldReplace(null, direct, cursorExpired = false))
    }

    @Test fun transportOrDeviceTransitionResetsCursor() {
        val remote = OwnerCursorNamespace(ConnectionMode.REMOTE_BACKEND, "KTDEAD")
        val otherDevice = OwnerCursorNamespace(ConnectionMode.DIRECT_BLE, "KTBEEF")
        assertNull(OwnerCursorPolicy.resume("92", remote, direct))
        assertNull(OwnerCursorPolicy.resume("92", otherDevice, direct))
    }

    @Test fun sameLiveNamespaceCanResumeUntilGap() {
        assertEquals("92", OwnerCursorPolicy.resume("92", direct, direct))
        assertFalse(OwnerCursorPolicy.shouldReplace(direct, direct, cursorExpired = false))
        assertTrue(OwnerCursorPolicy.shouldReplace(direct, direct, cursorExpired = true))
    }
}
