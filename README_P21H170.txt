P21H170 内核适配项目 - 文件索引
═══════════════════════════════════════════════════════════════════════════

【核心文件】

📄 设备树配置
   arch/arm64/boot/dts/sprd/ums512-1h170-overlay.dts
   - 完整的硬件设备树定义
   - 包含 WiFi、摄像头、LCD、电池、传感器等配置

📄 编译配置  
   arch/arm64/configs/ums512-p21h170_defconfig
   - 内核编译选项配置
   - 1914 行 UMS512 平台参数

📄 LCD 驱动
   arch/arm64/boot/dts/sprd/lcd/lcd_ft8201ab_boe_mipi_fhd_bbk_H170.dtsi
   - FT8201AB BOE 屏幕参数

【构建脚本】

📄 主编译脚本
   scripts/build-p21h170.sh
   - 支持: kernel | recovery | socko | all
   - 使用: ./scripts/build-p21h170.sh kernel

📄 Recovery 模板
   scripts/p21h170-recovery-anykernel.sh.in
   - Recovery 包生成模板

【文档】

📖 详细文档
   P21H170_ADAPTATION_SUMMARY.md
   - 完整的技术文档
   - 包含: 架构、配置、故障排除
   - 推荐阅读

📖 快速参考
   P21H170_QUICK_START.txt
   - 快速命令速查
   - 编译示例
   - 常见问题

📖 完成报告
   P21H170_COMPLETION_REPORT.txt
   - 工作总结
   - 技术指标
   - 后续步骤

【修复的文件】

✓ drivers/Kconfig - 删除 KernelSU 配置
✓ tools/lib/subcmd/subcmd-util.h - 修复 use-after-free
✓ include/linux/sprd_iommu.h - 修复函数签名
✓ scripts/dtc/Makefile - 禁用 YAML 支持

【预构建目录】

📁 prebuilts/p21h170/
   - 存放预编译工具

═══════════════════════════════════════════════════════════════════════════

【快速开始】

1. 编译内核
   cd /run/media/lwztime/GAME/A7/android_kernel_EEBBK_P21H180
   ./scripts/build-p21h170.sh kernel

2. 查看文档
   cat P21H170_ADAPTATION_SUMMARY.md      # 详细文档
   cat P21H170_QUICK_START.txt            # 快速参考
   cat P21H170_COMPLETION_REPORT.txt      # 完成报告

3. 验证文件
   ls -lh arch/arm64/boot/dts/sprd/ums512-1h170-overlay.dts
   ls -lh arch/arm64/configs/ums512-p21h170_defconfig

【关键命令】

编译内核:          ./scripts/build-p21h170.sh kernel
完整编译:          ./scripts/build-p21h170.sh all
仅生成 recovery:   ./scripts/build-p21h170.sh recovery

【支持联系】

问题排查: 查看 P21H170_ADAPTATION_SUMMARY.md 的【故障排除】
技术文档: 查看 P21H170_ADAPTATION_SUMMARY.md
快速帮助: 查看 P21H170_QUICK_START.txt

═══════════════════════════════════════════════════════════════════════════

✅ 所有文件已就绪，可以开始编译！
