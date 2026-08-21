package app.kitsu.mobile

import android.app.Application
import app.kitsu.mobile.auth.OidcConfiguration
import app.kitsu.mobile.auth.OidcSession
import app.kitsu.mobile.cache.EncryptedBoundedCache
import app.kitsu.mobile.connection.ConnectionCoordinator
import app.kitsu.mobile.connection.AndroidReconnectSuppressionStore
import app.kitsu.mobile.repository.OwnerRepository
import app.kitsu.mobile.relay.MobileRelayController
import app.kitsu.mobile.relay.MobileRelayDeviceSessionFactory
import app.kitsu.mobile.security.AndroidKeystoreCredentialStore
import app.kitsu.mobile.security.AndroidRemoteCompanionSelectionStore
import app.kitsu.mobile.transport.BackendConfiguration
import app.kitsu.mobile.transport.BackendKitsuTransport
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
    val oidc = OidcSession(
        application,
        OidcConfiguration(
            issuer = BuildConfig.KITSU_OIDC_ISSUER,
            clientId = BuildConfig.KITSU_OIDC_CLIENT_ID,
            redirectUri = BuildConfig.KITSU_OIDC_REDIRECT_URI,
        ),
        credentials,
    )

    private val bleConfiguration = BleGattConfiguration(
        service = UUID.fromString(BuildConfig.KITSU_BLE_SERVICE_UUID),
        write = UUID.fromString(BuildConfig.KITSU_BLE_WRITE_UUID),
        notify = UUID.fromString(BuildConfig.KITSU_BLE_NOTIFY_UUID),
    )
    private val direct = BleKitsuTransport(
        context = application,
        credentials = credentials,
        configuration = bleConfiguration,
    )
    private val remoteSelection = AndroidRemoteCompanionSelectionStore(application)
    private val backend = BackendKitsuTransport(
        configuration = BackendConfiguration(BuildConfig.KITSU_BACKEND_URL),
        tokens = oidc,
        selection = remoteSelection,
    )
    val mobileRelayController = MobileRelayController(
        context = application,
        credentials = credentials,
        backend = backend,
        enrollmentService = backend,
        companionCatalog = backend,
        sessions = MobileRelayDeviceSessionFactory { bond ->
            BleKitsuTransport(
                context = application,
                credentials = MobileRelayController.fixedBondCredentials(credentials, bond),
                configuration = bleConfiguration,
            )
        },
    )
    // Construct and synchronously load the non-secret stay-disconnected choice before any
    // MainViewModel init block can schedule an automatic connection.
    private val reconnectSuppressionStore = AndroidReconnectSuppressionStore(application)
    private val coordinator = ConnectionCoordinator(direct, backend, reconnectSuppressionStore)
    val ownerRepository = OwnerRepository(
        coordinator,
        EncryptedBoundedCache(application),
        backend,
        backend,
        direct,
        backend,
    )
}
