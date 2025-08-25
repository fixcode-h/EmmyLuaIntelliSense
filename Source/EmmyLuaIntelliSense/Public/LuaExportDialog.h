// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

/**
 * 简化的Lua导出对话框
 * 使用UE默认的消息对话框显示导出确认
 */
class EMMYLUAINTELLISENSE_API FLuaExportDialog
{
public:
    /** 显示导出确认对话框 */
    static void ShowExportConfirmation();
    
    /** 显示导出确认对话框（带文件数量） */
    static void ShowExportConfirmation(int32 FileCount);
};

/**
 * Lua导出通知管理器
 * 负责显示和管理导出相关的通知
 */
class EMMYLUAINTELLISENSE_API FLuaExportNotificationManager
{
public:
    /** 显示导出确认通知 */
    static TSharedPtr<SNotificationItem> ShowExportConfirmation(const FString& Message);

    /** 显示导出成功通知 */
    static TSharedPtr<SNotificationItem> ShowExportSuccess(const FString& Message);

    /** 显示导出失败通知 */
    static void ShowExportError(const FString& Message);

    /** 显示导出失败通知 */
    static TSharedPtr<SNotificationItem> ShowExportFailure(const FString& Message);

    /** 导出确认回调 */
    static void OnExportConfirmed();

    /** 导出跳过回调 */
    static void OnExportSkipped();

    /** 显示导出进度通知 */
    static TSharedPtr<SNotificationItem> ShowExportProgress(const FString& Message);

    /** 更新进度通知 */
    static void UpdateProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, float Progress);

    /** 完成进度通知 */
    static void CompleteProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, bool bSuccess);

private:
    /** 创建通知信息 */
    static FNotificationInfo CreateNotificationInfo(const FString& Message, SNotificationItem::ECompletionState CompletionState);

    /** 获取通知管理器 */
    static TSharedPtr<SNotificationList> GetNotificationList();
    
    /** 当前确认通知的引用 */
    static TSharedPtr<SNotificationItem> CurrentConfirmationNotification;
};