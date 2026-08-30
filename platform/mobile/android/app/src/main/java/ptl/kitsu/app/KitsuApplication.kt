package ptl.kitsu.app

import android.app.Application
import ptl.kitsu.app.cache.EncryptedBoundedCache
import ptl.kitsu.app.connection.AndroidReconnectSuppressionStore
import ptl.kitsu.app.connection.ConnectionCoordinator
import ptl.kitsu.app.repository.OwnerRepository
import ptl.kitsu.app.security.AndroidKeystoreCredentialStore
import ptl.kitsu.app.security.AndroidKeystoreMessageDraftStore
import ptl.kitsu.app.security.MessageDraftStore
import ptl.kitsu.app.notifications.AndroidKitsuNotificationSettingsStore
import ptl.kitsu.app.notifications.KitsuNotificationCoordinator
import ptl.kitsu.app.notifications.KitsuNotificationSettingsStore
import ptl.kitsu.app.transport.BleGattConfiguration
import ptl.kitsu.app.transport.BleKitsuTransport
import ptl.kitsu.app.walk.AndroidWalkStepSource
import ptl.kitsu.app.walk.WalkStepSource
import ptl.kitsu.app.automation.AutomationCapabilityStore
import java.util.UUID

open class KitsuApplication : Application() {
    lateinit var services: KitsuServiceContainer
        private set

    override fun onCreate() {
        super.onCreate()
        services = createServices()
    }

    /** Overridable only so instrumentation can supply a deterministic, offline transport. */
    protected open fun createServices(): KitsuServiceContainer = AppServices(this)
}

interface KitsuServiceContainer {
    val ownerRepository: OwnerRepository
    val messageDraftStore: MessageDraftStore
    val notificationSettingsStore: KitsuNotificationSettingsStore
    val walkStepSource: WalkStepSource
    val automationCapabilityStore: AutomationCapabilityStore
}

class AppServices(application: Application) : KitsuServiceContainer {
    val credentials = AndroidKeystoreCredentialStore(application)
    override val messageDraftStore: MessageDraftStore =
        AndroidKeystoreMessageDraftStore(application)
    override val notificationSettingsStore: KitsuNotificationSettingsStore =
        AndroidKitsuNotificationSettingsStore(application).also {
            // Continuity is a visible, process-local choice; never resurrect it after process death.
            it.resetConnectionContinuityForProcessStart()
        }
    override val walkStepSource: WalkStepSource = AndroidWalkStepSource(application)
    override val automationCapabilityStore: AutomationCapabilityStore =
        AutomationCapabilityStore(application)
    private val direct = BleKitsuTransport(
        context = application,
        credentials = credentials,
        configuration = BleGattConfiguration(
            service = UUID.fromString(BuildConfig.KITSU_BLE_SERVICE_UUID),
            write = UUID.fromString(BuildConfig.KITSU_BLE_WRITE_UUID),
            notify = UUID.fromString(BuildConfig.KITSU_BLE_NOTIFY_UUID),
        ),
    )
    private val coordinator = ConnectionCoordinator(
        direct,
        AndroidReconnectSuppressionStore(application),
    )
    init {
        direct.setDisconnectObserver(coordinator::onDirectTransportDisconnected)
    }
    override val ownerRepository = OwnerRepository(
        coordinator = coordinator,
        cache = EncryptedBoundedCache(application),
        credentials = credentials,
        pairingService = direct,
    )
    @Suppress("unused")
    private val notificationCoordinator = KitsuNotificationCoordinator(
        application = application,
        repository = ownerRepository,
        settingsStore = notificationSettingsStore,
    )
}
