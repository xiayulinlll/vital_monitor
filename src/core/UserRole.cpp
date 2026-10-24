#include "UserRole.h"

QString userRoleName(UserRole r)
{
    switch (r) {
    case UserRole::Viewer:   return QStringLiteral("查看者");
    case UserRole::Operator: return QStringLiteral("操作者");
    case UserRole::Admin:    return QStringLiteral("管理员");
    }
    return QStringLiteral("未知");
}

QString permissionName(Permission p)
{
    switch (p) {
    case Permission::ViewData:         return QStringLiteral("查看数据");
    case Permission::AcknowledgeAlarm: return QStringLiteral("确认 / 暂停告警");
    case Permission::ManageData:       return QStringLiteral("导入 / 导出 / 清空数据");
    case Permission::ModifyConfig:     return QStringLiteral("修改量程与报警限");
    case Permission::ManageUsers:      return QStringLiteral("切换用户角色");
    }
    return QStringLiteral("未知权限");
}

bool roleHasPermission(UserRole role, Permission p)
{
    // 每个权限对应的最低角色要求。
    UserRole required = UserRole::Viewer;
    switch (p) {
    case Permission::ViewData:         required = UserRole::Viewer;   break;
    case Permission::AcknowledgeAlarm: required = UserRole::Operator; break;
    case Permission::ManageData:       required = UserRole::Operator; break;
    case Permission::ModifyConfig:     required = UserRole::Admin;    break;
    case Permission::ManageUsers:      required = UserRole::Admin;    break;
    }

    // 角色是线性等级的枚举：Admin(2) > Operator(1) > Viewer(0)，
    // 因此「当前等级 >= 要求等级」即可，无需展开权限矩阵。
    return static_cast<int>(role) >= static_cast<int>(required);
}
