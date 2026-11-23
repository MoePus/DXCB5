// Distributed under the University of Illinois Open Source License.
using UnityEditor;
using UnityEngine;

namespace MoePus.Dxcb5.Editor
{
    public sealed class Dxcb5Window : EditorWindow
    {
#if UNITY_EDITOR_WIN
        private Dxcb5HookBridge.HookStats _stats;
#endif
        private string _lastFeedback;
        private MessageType _lastFeedbackType = MessageType.None;

        [MenuItem("Tools/DXCB5 Shader Compiler")]
        public static void ShowWindow()
        {
            var win = GetWindow<Dxcb5Window>("DXCB5 Compiler");
            win.minSize = new Vector2(460f, 360f);
            win.Refresh();
            win.Show();
        }

        private void OnEnable() => Refresh();
        private void OnFocus() => Refresh();

        private void Refresh()
        {
#if UNITY_EDITOR_WIN
            _stats = Dxcb5HookBridge.GetStats();
#endif
            Repaint();
        }

        private void OnGUI()
        {
            EditorGUILayout.Space(6);
            EditorGUILayout.LabelField("DXCB5 SM5.0 Shader Compiler", EditorStyles.boldLabel);

            string unityVer = Application.unityVersion;
            bool isRecommended = unityVer == "2022.3.22f1";
            EditorGUILayout.HelpBox(
                $"Unity Editor: {unityVer} {(isRecommended ? "(VRChat recommended)" : "(2022.3.x compatible)")}",
                MessageType.Info);

#if !UNITY_EDITOR_WIN
            EditorGUILayout.HelpBox("DXCB5 requires Windows x64 Unity Editor.", MessageType.Error);
            return;
#else
            DrawStatusSection();
            EditorGUILayout.Space(8);
            DrawOptionsSection();
            EditorGUILayout.Space(8);
            DrawStatsSection();

            if (!string.IsNullOrEmpty(_lastFeedback))
            {
                EditorGUILayout.Space(6);
                EditorGUILayout.HelpBox(_lastFeedback, _lastFeedbackType);
            }
#endif
        }

#if UNITY_EDITOR_WIN
        private void DrawStatusSection()
        {
            EditorGUILayout.LabelField("Status", EditorStyles.boldLabel);
            bool installed = Dxcb5HookBridge.IsInstalled;
            bool hasPayloads = Dxcb5HookBridge.HasNativePayloads(out string missingDetail);

            if (installed)
            {
                string cacheNote = _stats.SnippetHookActive
                    ? "In-process RAM variant cache active."
                    : "Worker CgBatch plugin active (in-process RAM cache inactive).";
                EditorGUILayout.HelpBox(
                    $"Status: ACTIVE ({(Dxcb5HookBridge.Enabled ? "DXCB5 enabled" : "paused -> stock FXC")})\n{cacheNote}",
                    MessageType.Info);
            }
            else
            {
                EditorGUILayout.HelpBox(
                    "Status: NOT ACTIVE (Unity is using stock FXC).",
                    MessageType.Warning);
            }

            if (!hasPayloads)
                EditorGUILayout.HelpBox(missingDetail, MessageType.Error);

            using (new EditorGUILayout.HorizontalScope())
            {
                using (new EditorGUI.DisabledScope(!hasPayloads))
                {
                    string btn = installed ? "Reinstall Hook" : "Activate DXCB5";
                    if (GUILayout.Button(btn, GUILayout.Height(26)))
                    {
                        bool ok = Dxcb5HookBridge.TryInstall(Dxcb5HookBridge.SnippetCacheEnabled);
                        _lastFeedback = ok ? "DXCB5 hook activated." : "Failed to activate DXCB5 hook (see Console).";
                        _lastFeedbackType = ok ? MessageType.Info : MessageType.Error;
                        Refresh();
                    }
                }
                using (new EditorGUI.DisabledScope(!installed))
                {
                    if (GUILayout.Button("Reload Workers", GUILayout.Height(26)))
                    {
                        Dxcb5HookBridge.ReloadWorkers();
                        _lastFeedback = "Restarted UnityShaderCompiler workers and cleared RAM cache (updated dxcompiler.dll / plugin will load on next compile).";
                        _lastFeedbackType = MessageType.Info;
                        Refresh();
                    }
                    if (GUILayout.Button("Deactivate", GUILayout.Height(26)))
                    {
                        Dxcb5HookBridge.SafeUninstall();
                        _lastFeedback = "DXCB5 hook deactivated (stock FXC restored for new compiles).";
                        _lastFeedbackType = MessageType.Info;
                        Refresh();
                    }
                }
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                if (GUILayout.Button("Clear RAM Variant Cache"))
                {
                    Dxcb5HookBridge.ClearCache();
                    _lastFeedback = "Cleared in-process RAM variant cache.";
                    _lastFeedbackType = MessageType.Info;
                    Refresh();
                }
                if (GUILayout.Button("Clear Project Library/ShaderCache"))
                {
                    if (EditorUtility.DisplayDialog(
                            "Clear ShaderCache",
                            "Delete Library/ShaderCache in this project so shaders recompile using DXCB5?",
                            "Clear", "Cancel"))
                    {
                        bool ok = Dxcb5HookBridge.ClearProjectShaderCache(out string msg);
                        _lastFeedback = msg;
                        _lastFeedbackType = ok ? MessageType.Info : MessageType.Error;
                        Refresh();
                    }
                }
            }
        }

        private void DrawOptionsSection()
        {
            EditorGUILayout.LabelField("Runtime Options (takes effect immediately)", EditorStyles.boldLabel);
            bool enabled = EditorGUILayout.ToggleLeft(
                "Enable DXCB5 for vs_5_0 / ps_5_0 (uncheck to route directly to FXC)",
                Dxcb5HookBridge.Enabled);
            if (enabled != Dxcb5HookBridge.Enabled)
            {
                Dxcb5HookBridge.Enabled = enabled;
                Refresh();
            }

            bool nanSafe = EditorGUILayout.ToggleLeft(
                "Strict NaN Semantics (--nan-safe, slightly larger bytecode)",
                Dxcb5HookBridge.NanSafe);
            if (nanSafe != Dxcb5HookBridge.NanSafe)
            {
                Dxcb5HookBridge.NanSafe = nanSafe;
                Refresh();
            }

            bool ramCache = EditorGUILayout.ToggleLeft(
                "In-Process RAM Variant Deduplication Cache (Unity 2022.3.22f1)",
                Dxcb5HookBridge.SnippetCacheEnabled);
            if (ramCache != Dxcb5HookBridge.SnippetCacheEnabled)
            {
                Dxcb5HookBridge.SnippetCacheEnabled = ramCache;
                Refresh();
            }
        }

        private void DrawStatsSection()
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("Session Statistics", EditorStyles.boldLabel);
                GUILayout.FlexibleSpace();
                if (GUILayout.Button("Refresh", GUILayout.Width(72)))
                    Refresh();
            }

            ulong total = _stats.RamCacheHits + _stats.IpcCompiles;
            double hitRate = total > 0 ? (100.0 * _stats.RamCacheHits / total) : 0.0;
            EditorGUILayout.LabelField(
                $"Workers Hooked: {_stats.WorkersHooked}   |   RAM Hits: {_stats.RamCacheHits}   |   IPC Compiles: {_stats.IpcCompiles}   |   RAM Hit Rate: {hitRate:F1}%");
        }
#endif
    }
}
