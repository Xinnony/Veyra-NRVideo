// Audio filters: live dubbing of the translated dialogue (DubVoice).
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

DialogLayer {
    id: root
    objectName: "audio-filters-dialog"
    key: "audioFilters"
    glyph: "music"
    title: qsTr("音频滤镜")
    dialogWidth: 580
    actions: [{ label: qsTr("实时翻译设置…") }, { label: qsTr("完成"), primary: true }]
    onActionTriggered: label => {
        if (label === qsTr("实时翻译设置…")) dialogHost.open("translation")
        else dialogHost.close()
    }

    readonly property var dub: veyra.dubVoice || null
    readonly property var translation: veyra.liveTranslation || null
    readonly property bool openAi: dub !== null && dub.provider === 1
    readonly property var providers: [
        {id: "0", label: qsTr("Windows 语音（本机，免费）")},
        {id: "1", label: qsTr("OpenAI 语音（需要 API 密钥）")}
    ]
    readonly property var windowsVoiceOptions: [{id: "-1", label: qsTr("自动（按译文语言）")}]
        .concat((dub ? dub.windowsVoices : []).map((name, i) => ({id: String(i), label: name})))
    readonly property var openAiVoices: ["alloy", "ash", "ballad", "coral", "echo", "fable", "nova", "onyx", "sage", "shimmer", "verse"]
        .map(v => ({id: v, label: v}))
    function optionLabel(options, value) {
        return options.find(option => option.id === value)?.label || value
    }

    DialogSection { text: qsTr("实时配音") }
    DialogGroup {
        VRow {
            label: qsTr("朗读翻译并压低原声人声")
            hintBelow: true
            hint: qsTr("把实时翻译的每句话用语音读出来。配音会比原声晚 1–3 秒；游戏画面不会被延迟。需要开启实时翻译，识别来源为 Veyra 音频。")
            VSwitch {
                objectName: "audio-filters-enabled"
                enabled: root.dub !== null
                checked: root.dub ? root.dub.enabled : false
                onToggled: value => { root.dub.enabled = value }
            }
        }
        VRow {
            label: qsTr("原声 ↔ 配音")
            hintBelow: true
            hint: qsTr("向左只保留原声人声，居中两者都保留，向右只保留配音。车辆、枪声、动物、警笛等音效不受这个滑块影响。")
            VSlider {
                objectName: "audio-filters-balance"
                implicitWidth: 220
                from: 0; to: 1; inputScale: 100
                defaultValue: 0.75
                value: root.dub ? root.dub.balance : 0.75
                onMoved: value => { root.dub.balance = value }
            }
        }
        VRow {
            label: qsTr("配音音量")
            VSlider {
                objectName: "audio-filters-volume"
                implicitWidth: 220
                from: 0; to: 2; inputScale: 100
                defaultValue: 1
                value: root.dub ? root.dub.volume : 1
                onMoved: value => { root.dub.volume = value }
            }
        }
        Text {
            objectName: "audio-filters-reduction"
            Layout.fillWidth: true
            visible: root.dub !== null && root.dub.enabled
            text: !root.dub ? "" : root.dub.reduction === 1
                ? qsTr("多声道音频：只压低中置声道，那里通常是对白；不增加延迟。")
                : root.dub.reduction === 2
                ? qsTr("立体声音频：压低居中的人声频段（不是 AI 分离，居中的音效也会略微变小），声音延迟约 21 ms。")
                : qsTr("这个声音无法分离人声（单声道），只叠加配音。")
            color: Theme.t2
            font.family: Theme.fontUi
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }

    DialogSection { text: qsTr("配音语音") }
    DialogGroup {
        VRow {
            label: qsTr("语音引擎")
            VSelect {
                objectName: "audio-filters-provider"
                implicitWidth: 248
                options: root.providers
                value: root.optionLabel(options, String(root.dub ? root.dub.provider : 0))
                onPicked: id => { root.dub.provider = Number(id) }
            }
        }
        VRow {
            visible: !root.openAi
            label: qsTr("Windows 语音")
            hintBelow: true
            hint: qsTr("可在 Windows 设置 › 时间和语言 › 语音 中添加其他语言的语音。")
            VSelect {
                objectName: "audio-filters-windows-voice"
                implicitWidth: 248
                options: root.windowsVoiceOptions
                value: root.optionLabel(options, String(root.dub ? root.dub.windowsVoice : -1))
                onPicked: id => { root.dub.windowsVoice = Number(id) }
            }
        }
        VRow {
            visible: root.openAi
            label: qsTr("接口模型名")
            DialogTextField {
                implicitWidth: 248
                text: root.dub ? root.dub.openAiModel : ""
                onTyped: value => { root.dub.openAiModel = value }
            }
        }
        VRow {
            visible: root.openAi
            label: qsTr("声音")
            VSelect {
                objectName: "audio-filters-openai-voice"
                implicitWidth: 248
                options: root.openAiVoices
                value: root.dub ? root.dub.openAiVoice : "coral"
                onPicked: id => { root.dub.openAiVoice = id }
            }
        }
        VRow {
            visible: root.openAi
            label: "API Key"
            hintBelow: true
            hint: qsTr("ChatGPT 订阅不包含 API 额度：请在 platform.openai.com 创建密钥，按用量计费。仅本次运行有效。")
            DialogTextField {
                implicitWidth: 248
                echoMode: TextInput.Password
                onTyped: value => { if (root.dub) root.dub.setApiKey(value) }
            }
        }
        VRow {
            label: qsTr("试听")
            VButton {
                objectName: "audio-filters-preview"
                enabled: root.dub !== null && root.dub.enabled
                text: qsTr("播放示例")
                onClicked: root.dub.preview()
            }
        }
    }
    Text {
        objectName: "audio-filters-status"
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.dub ? root.dub.status : ""
        color: Theme.t2
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsBody
        wrapMode: Text.Wrap
    }
    Text {
        Layout.fillWidth: true
        visible: root.dub !== null && root.dub.enabled && (root.dub.spoken > 0 || root.dub.skipped > 0)
        text: !root.dub ? "" : qsTr("已配音 %1 句 · 跳过 %2 句 · 平均晚于原声 %3 ms")
            .arg(root.dub.spoken).arg(root.dub.skipped).arg(Math.round(root.dub.averageDelayMs))
        color: Theme.t3
        font.family: Theme.fontUi
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }
}
