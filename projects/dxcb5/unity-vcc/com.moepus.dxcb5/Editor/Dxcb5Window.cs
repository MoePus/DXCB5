using UnityEditor;
using UnityEngine;

namespace MoePus.Dxcb5.Editor
{
    public sealed class Dxcb5Window : EditorWindow
    {
        private Dxcb5Config _config;
        private Dxcb5StatsSummary _stats;
        private Vector2 _fallbackScroll;
        private string _lastFeedback;
        private MessageType _lastFeedbackType = MessageType.None;

        [MenuItem("Tools/DXCB5 Shader Compiler")]
        public static void ShowWindow()
        {
            var win = GetWindow<Dxcb5Window>("DXCB5 Compiler");
            win.minSize = new Vector2(460f, 420f);
            win.Refresh();
            win.Show();
        }

        private void OnEnable()
        {
            Refresh();
        }

        private void OnFocus()
        {
            Refresh();
        }

        private void Refresh()
        {
            _config = Dxcb5Installer.LoadConfig();
            _stats = Dxcb5Installer.LoadStats();
            Repaint();
        }

        private void OnGUI()
        {
            EditorGUILayout.Space(6);
            EditorGUILayout.LabelField("DXCB5 SM5.0 Shader Compiler (Unity D3D11 Proxy)", EditorStyles.boldLabel);

            string unityVer = Application.unityVersion;
            bool isRecommended = unityVer == "2022.3.22f1";
            EditorGUILayout.HelpBox(
                $"Unity Editor: {unityVer} {(isRecommended ? "(VRChat recommended)" : "(2022.3.x compatible)")}\n" +
                $"Tools Dir: {Dxcb5Installer.GetUnityToolsDir()}",
                MessageType.Info);

            DrawInstallSection();
            EditorGUILayout.Space(8);
            DrawConfigSection();
            EditorGUILayout.Space(8);
            DrawStatsSection();

            if (!string.IsNullOrEmpty(_lastFeedback))
            {
                EditorGUILayout.Space(6);
                EditorGUILayout.HelpBox(_lastFeedback, _lastFeedbackType);
            }
        }

        private void DrawInstallSection()
        {
            EditorGUILayout.LabelField("Installation", EditorStyles.boldLabel);
            Dxcb5InstallState state = Dxcb5Installer.GetInstallState();
            bool hasPayloads = Dxcb5Installer.HasNativePayloads(out string missingDetail);

            switch (state)
            {
                case Dxcb5InstallState.Installed:
                    EditorGUILayout.HelpBox(
                        "Status: INSTALLED (D3DCompiler_47.dll proxy active; original FXC backed up as D3DCompiler_47_fxc.dll).",
                        MessageType.Info);
                    break;
                case Dxcb5InstallState.NotInstalled:
                    EditorGUILayout.HelpBox(
                        "Status: NOT INSTALLED (Unity is using stock FXC D3DCompiler_47.dll).",
                        MessageType.Warning);
                    break;
                case Dxcb5InstallState.PartialOrCorrupt:
                    EditorGUILayout.HelpBox(
                        "Status: PARTIAL INSTALL DETECTED. Click 'Uninstall & Restore FXC' or reinstall.",
                        MessageType.Error);
                    break;
                default:
                    EditorGUILayout.HelpBox(
                        "Status: Unsupported platform (Windows x64 Unity Editor required).",
                        MessageType.Error);
                    return;
            }

            if (!hasPayloads)
            {
                EditorGUILayout.HelpBox(missingDetail, MessageType.Warning);
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                using (new EditorGUI.DisabledScope(!hasPayloads))
                {
                    string installLabel = state == Dxcb5InstallState.Installed
                        ? "Reinstall / Update DXCB5 Proxy"
                        : "Install DXCB5 Proxy";
                    if (GUILayout.Button(installLabel, GUILayout.Height(28)))
                    {
                        if (Dxcb5Installer.InstallProxy(out string msg))
                        {
                            _lastFeedback = msg;
                            _lastFeedbackType = MessageType.Info;
                        }
                        else
                        {
                            _lastFeedback = msg;
                            _lastFeedbackType = MessageType.Error;
                        }
                        Refresh();
                    }
                }

                using (new EditorGUI.DisabledScope(state == Dxcb5InstallState.NotInstalled))
                {
                    if (GUILayout.Button("Uninstall & Restore FXC", GUILayout.Height(28)))
                    {
                        if (Dxcb5Installer.UninstallProxy(out string msg))
                        {
                            _lastFeedback = msg;
                            _lastFeedbackType = MessageType.Info;
                        }
                        else
                        {
                            _lastFeedback = msg;
                            _lastFeedbackType = MessageType.Error;
                        }
                        Refresh();
                    }
                }
            }

            if (GUILayout.Button("Clear Project Library/ShaderCache"))
            {
                if (EditorUtility.DisplayDialog(
                        "Clear ShaderCache",
                        "Delete Library/ShaderCache in this project so shaders recompile using DXCB5?",
                        "Clear", "Cancel"))
                {
                    bool ok = Dxcb5Installer.ClearProjectShaderCache(out string msg);
                    _lastFeedback = msg;
                    _lastFeedbackType = ok ? MessageType.Info : MessageType.Error;
                }
            }
        }

        private void DrawConfigSection()
        {
            EditorGUILayout.LabelField("Runtime Options (takes effect immediately)", EditorStyles.boldLabel);
            EditorGUI.BeginChangeCheck();
            _config.Enabled = EditorGUILayout.ToggleLeft("Enable DXCB5 for vs_5_0 / ps_5_0 (uncheck to route directly to FXC)", _config.Enabled);
            _config.RequirePackage = EditorGUILayout.ToggleLeft("Project-scoped only (auto-revert to FXC if com.moepus.dxcb5 is removed from VCC)", _config.RequirePackage);
            _config.NanSafe = EditorGUILayout.ToggleLeft("Strict NaN Semantics (--nan-safe, slightly larger bytecode)", _config.NanSafe);
            _config.LogStats = EditorGUILayout.ToggleLeft("Record Compile Telemetry (stats.log)", _config.LogStats);
            if (EditorGUI.EndChangeCheck())
            {
                Dxcb5Installer.SaveConfig(_config);
            }
        }

        private void DrawStatsSection()
        {
            using (new EditorGUILayout.HorizontalScope())
            {
                EditorGUILayout.LabelField("Compile Telemetry", EditorStyles.boldLabel);
                GUILayout.FlexibleSpace();
                if (GUILayout.Button("Refresh", GUILayout.Width(72)))
                    Refresh();
                if (GUILayout.Button("Clear Log", GUILayout.Width(78)))
                {
                    Dxcb5Installer.ClearStatsLog();
                    Refresh();
                }
            }

            int total = _stats.Hits + _stats.Fallbacks;
            double rate = total > 0 ? (100.0 * _stats.Hits / total) : 0.0;
            EditorGUILayout.LabelField(
                $"DXCB5 Hits: {_stats.Hits}   |   FXC Fallbacks: {_stats.Fallbacks}   |   Hit Rate: {rate:F1}%   |   Avg DXCB5: {_stats.AvgHitMs:F1} ms");

            if (_stats.RecentFallbacks != null && _stats.RecentFallbacks.Count > 0)
            {
                EditorGUILayout.LabelField("Recent FXC Fallbacks:", EditorStyles.miniBoldLabel);
                _fallbackScroll = EditorGUILayout.BeginScrollView(_fallbackScroll, GUILayout.MinHeight(90), GUILayout.MaxHeight(160));
                for (int i = _stats.RecentFallbacks.Count - 1; i >= 0; --i)
                {
                    var fb = _stats.RecentFallbacks[i];
                    EditorGUILayout.LabelField($"[{fb.Target} {fb.Entrypoint}] {fb.Reason}", EditorStyles.wordWrappedMiniLabel);
                }
                EditorGUILayout.EndScrollView();
            }
        }
    }
}
