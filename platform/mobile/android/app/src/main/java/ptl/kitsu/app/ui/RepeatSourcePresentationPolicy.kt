package ptl.kitsu.app.ui

import java.util.Base64
import ptl.kitsu.app.model.MeshPeerKeyPolicy
import ptl.kitsu.app.model.LastFloodAdvert
import ptl.kitsu.app.model.Message
import ptl.kitsu.app.model.Peer
import ptl.kitsu.app.model.RepeatSource

/** Resolves unauthenticated path tokens only against distinct authenticated full peer keys. */
internal object RepeatSourcePresentationPolicy {
    fun visibleLine(message: Message, peers: List<Peer>): String? {
        return visibleLine(
            repeatCount = message.repeatCount,
            sources = message.repeatSources,
            sourcesTruncated = message.repeatSourcesTruncated,
            peers = peers,
        )
    }

    fun visibleLine(evidence: LastFloodAdvert, peers: List<Peer>): String? = visibleLine(
        repeatCount = evidence.repeatCount,
        sources = evidence.repeatSources,
        sourcesTruncated = evidence.repeatSourcesTruncated,
        peers = peers,
    )

    private fun visibleLine(
        repeatCount: Int?,
        sources: List<RepeatSource>?,
        sourcesTruncated: Boolean?,
        peers: List<Peer>,
    ): String? {
        if (repeatCount?.let { it > 0 } != true) return null
        val source = sources?.singleOrNull()
        val likely = source?.let { uniquelyMatchedPeer(it, peers) }
            ?.name
            ?.takeIf(String::isNotBlank)
        return if (likely != null && sourcesTruncated == false) {
            "Returned repeat heard · likely $likely"
        } else {
            "Returned repeat heard"
        }
    }

    fun detail(message: Message, peers: List<Peer>): String? {
        return detail(
            repeatCount = message.repeatCount,
            sources = message.repeatSources,
            sourcesTruncated = message.repeatSourcesTruncated,
            peers = peers,
        )
    }

    fun detail(evidence: LastFloodAdvert, peers: List<Peer>): String? = detail(
        repeatCount = evidence.repeatCount,
        sources = evidence.repeatSources,
        sourcesTruncated = evidence.repeatSourcesTruncated,
        peers = peers,
    )

    private fun detail(
        repeatCount: Int?,
        sources: List<RepeatSource>?,
        sourcesTruncated: Boolean?,
        peers: List<Peer>,
    ): String? {
        if (repeatCount?.let { it > 0 } != true) return null
        sources ?: return null
        val scope = "These unauthenticated local path tokens are not a list of all repeaters or recipients."
        if (sources.isEmpty()) {
            return "No last-hop path token was retained for the returned copies heard locally. $scope"
        }
        if (sources.size == 1) {
            val source = sources.single()
            val likely = uniquelyMatchedPeer(source, peers)?.name?.takeIf(String::isNotBlank)
            val resolution = if (likely == null) {
                "It does not uniquely match one current authenticated full-key peer."
            } else {
                "It uniquely matches one current authenticated full-key peer, likely $likely."
            }
            return "A returned copy heard locally carried last-hop path token ${source.lastHopToken}. " +
                "$resolution $scope${truncation(sourcesTruncated)}"
        }
        return "Kitsu recorded ${sources.size} distinct last-hop path tokens on returned copies heard locally. " +
            "$scope${truncation(sourcesTruncated)}"
    }

    private fun truncation(sourcesTruncated: Boolean?): String = if (sourcesTruncated == true) {
        " Only the first four distinct local path tokens are retained."
    } else {
        ""
    }

    private fun uniquelyMatchedPeer(source: RepeatSource, peers: List<Peer>): Peer? {
        val token = decodeToken(source.lastHopToken) ?: return null
        val matches = peers.asSequence()
            .filter { peer -> MeshPeerKeyPolicy.isCanonicalBase64Url(peer.id) }
            .distinctBy(Peer::id)
            .filter { peer ->
                val fullKey = runCatching { Base64.getUrlDecoder().decode(peer.id) }.getOrNull()
                fullKey != null && fullKey.size == 32 &&
                    token.indices.all { index -> fullKey[index] == token[index] }
            }
            .take(2)
            .toList()
        return matches.singleOrNull()
    }

    private fun decodeToken(token: String): ByteArray? {
        if (!TOKEN.matches(token)) return null
        return runCatching {
            ByteArray(token.length / 2) { index ->
                token.substring(index * 2, index * 2 + 2).toInt(16).toByte()
            }
        }.getOrNull()
    }

    private val TOKEN = Regex("^(?:[0-9A-F]{2}){1,3}$")
}
