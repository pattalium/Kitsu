package ptl.kitsu.app.automation

import android.app.Activity
import android.os.Bundle

/**
 * Narrow Tasker/automation entry point. It never performs a BLE action itself;
 * a valid capability opens an explicit in-app confirmation route.
 */
class KitsuAutomationActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val request = KitsuAutomationPolicy.parse(intent)
        val accepted = request != null &&
            AutomationCapabilityStore(applicationContext).accepts(request.capabilityToken)
        if (accepted) {
            startActivity(KitsuAutomationPolicy.confirmationIntent(packageName, request!!.action))
            setResult(RESULT_OK)
        } else {
            setResult(RESULT_CANCELED)
        }
        finish()
    }
}
