import QtQuick

VMenu {
    id: menu
    required property var backend
    signal settingsRequested()
    signal filtersRequested()

    readonly property bool captureSource: backend.sourceKind === "capture"
    readonly property bool monitorOff: captureSource && backend.preferences.captureAudioMonitor === false
    readonly property string buttonLabel: captureSource
        ? (monitorOff ? qsTr("本机音频监听") + " · " + qsTr("已关闭") : qsTr("本机音频监听"))
        : qsTr("音轨")

    title: captureSource ? qsTr("声音") : qsTr("音轨")
    items: {
        const settings = { label: qsTr("音频设置…"), note: qsTr("输出设备、音画同步、偏移"), icon: "music", act: "settings" }
        const filters = { label: qsTr("音频滤镜…"), note: qsTr("实时配音、压低原声人声"), icon: "music", act: "filters",
                          checked: !!(backend.dubVoice && backend.dubVoice.enabled) }
        if (captureSource) {
            return [
                { label: qsTr("本机音频监听"), tag: monitorOff ? qsTr("已关闭") : "", icon: "vol",
                  checked: !monitorOff, gold: monitorOff, act: "monitor", enabledValue: monitorOff },
                { sep: true }, settings, filters
            ]
        }
        const tracks = backend.audioTracks
        if (tracks.length === 0)
            return [{ label: qsTr("片源没有音轨或尚未打开"), disabled: true }]
        return tracks.map(t => ({ label: t.label, index: t.index,
            note: t.channels > 0 ? t.channels + qsTr(" 声道") : "",
            checked: t.index === backend.selectedAudioTrack })).concat([{ sep: true }, settings, filters])
    }

    onPicked: (index, item) => {
        if (!item) return
        if (item.act === "settings") {
            settingsRequested()
        } else if (item.act === "filters") {
            filtersRequested()
        } else if (item.act === "monitor") {
            if (captureSource) backend.setPreference("captureAudioMonitor", item.enabledValue)
        } else if (item.index !== undefined) {
            backend.selectedAudioTrack = item.index
        }
    }
}
