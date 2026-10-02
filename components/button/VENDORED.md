# 本地 vendored button 组件(勿直接升级)

来源: espressif/button 4.2.1 (esp-iot-solution master, 2026-10) +
button_adc.c 携带 AI Passport 的 ADC 抗腐蚀补丁(BLE 射频窗导致 SAR ADC
读数≈0mV 的假按下/假释放防护,详见 button_adc.c 内注释)。

为什么 vendored: 补丁针对注册表 4.2.1 原文件,组件管理器无法承载修改过的
managed_components(哈希校验/覆盖冲突,CI 已两次踩坑)。本地同名组件优先
满足 bsp 的 `espressif/button: "*"` 依赖,构建不再联网解析此组件。

升级方式: 先在 button_adc.c 之上 rework 补丁,真机回归 KEYDBG 取证流程。
