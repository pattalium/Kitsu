package app.kitsu.mobile

import android.app.Application
import app.kitsu.mobile.cache.EncryptedBoundedCache
import app.kitsu.mobile.connection.AndroidReconnectSuppressionStore
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.repository.OwnerRepository
import app.kitsu.mobile.security.AndroidKeystoreCredentialStore
import app.kitsu.mobile.transport.BleGattConfiguration
import app.kitsu.mobile.transport.BleKitsuTransport
import java.util.UUID

class KitsuApplication : Application() {
    lateinit var services: AppServices
        private set

    override fun onCreate() {
        super.onCreate()
        services = AppServices(this)
    }
}

class AppServices(application: Application) {
    val credentials = AndroidKeystoreCredentialStore(application)
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
    val ownerRepository = OwnerRepository(
        coordinator = coordinator,
        cache = EncryptedBoundedCache(application),
        credentials = credentials,
        pairingService = direct,
    )
}
