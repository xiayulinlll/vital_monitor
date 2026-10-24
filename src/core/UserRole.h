#ifndef USERROLE_H
#define USERROLE_H

#include <QString>

/**
 * @brief 用户身份与权限模型（对应优化文档 8.2 第 2 项）。
 *
 * 现存问题：「无用户身份认证与权限控制（任何人可修改报警限）」。
 * 临床报警限被非授权人员改动是已知的重大误用风险（IEC 62366），
 * 因此把操作分成三级，并对越权操作同时做**拦截 + 审计**。
 *
 * 三级权限（由低到高，高权限隐含低权限能力）：
 *  - Viewer（查看者）：只能查看趋势与报警，不能改任何配置；
 *  - Operator（操作者，默认）：可确认 / 暂停告警、导入导出、清空数据 ——
 *    这些是值班医护的日常操作；
 *  - Admin（管理员）：额外允许修改量程 / 报警限 / 患者类型 ——
 *    这些改动会**直接改变报警行为**，属于必须授权的高风险操作。
 *
 * 设计取舍：本演示**不做密码认证**（无密钥 / 证书体系，做出来也只是摆设），
 * 只演示「角色 → 能力」的判定与审计闭环；生产环境应接入
 * 账号口令 / 工牌刷卡 / 医院统一身份认证（AD / LDAP）。
 */
enum class UserRole {
    Viewer = 0,   ///< 查看者
    Operator,     ///< 操作者（默认）
    Admin         ///< 管理员
};

/// 角色显示名。
QString userRoleName(UserRole r);

/**
 * @brief 需要权限保护的敏感操作。
 *
 * 只列出会改变**报警行为**或**数据留存**的操作；
 * 纯粹的视图操作（缩放、跟随、跨度）不需要权限。
 */
enum class Permission {
    ViewData = 0,      ///< 查看趋势 / 数值 / 报警（所有角色均可）
    AcknowledgeAlarm,  ///< 确认 / 暂停告警（Operator 及以上）
    ManageData,        ///< 导入 / 导出 / 清空数据（Operator 及以上）
    ModifyConfig,      ///< 修改量程 / 报警限 / 患者类型（Admin）
    ManageUsers        ///< 切换用户角色（Admin）
};

/// 权限显示名（用于提示与审计）。
QString permissionName(Permission p);

/**
 * @brief 判断某角色是否具备某权限。
 *
 * 采用「等级越高包含越多」的线性模型，避免把权限矩阵写死在
 * 每个调用点（漏写一处就是一个越权漏洞）。
 */
bool roleHasPermission(UserRole role, Permission p);

#endif // USERROLE_H
