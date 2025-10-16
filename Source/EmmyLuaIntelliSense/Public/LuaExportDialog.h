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
    
    /** 显示导出确认对话框（分别显示蓝图和原生类型数量） */
    static void ShowExportConfirmation(int32 BlueprintCount, int32 NativeTypeCount);
    
    /** 显示导出确认对话框（分别显示蓝图、原生类型和核心文件数量） */
    static void ShowExportConfirmation(int32 BlueprintCount, int32 NativeTypeCount, int32 CoreFileCount);
    
    /** 显示扫描确认对话框（询问用户是否开始扫描） */
    static void ShowScanConfirmation();
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
    
    /** 显示扫描确认通知 */
    static TSharedPtr<SNotificationItem> ShowScanConfirmation(const FString& Message);

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
    
    /** 扫描确认回调 */
    static void OnScanConfirmed();
    
    /** 扫描跳过回调 */
    static void OnScanSkipped();

    /** 显示导出进度通知 */
    static TSharedPtr<SNotificationItem> ShowExportProgress(const FString& Message);

    /** 更新进度通知 */
    static void UpdateProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, float Progress);

    /** 完成进度通知 */
    static void CompleteProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, bool bSuccess);

    /** 显示扫描进度通知（带取消按钮） */
    static TSharedPtr<SNotificationItem> ShowScanProgress(const FString& Message);

    /** 更新扫描进度通知 */
    static void UpdateScanProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, float Progress);

    /** 完成扫描进度通知 */
    static void CompleteScanProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, bool bSuccess);

    /** 扫描取消回调 */
    static void OnScanCancelled();
    
    /** 清理所有通知和定时器（模块卸载时调用） */
    static void Cleanup();

private:
    /** 创建通知信息 */
    static FNotificationInfo CreateNotificationInfo(const FString& Message, SNotificationItem::ECompletionState CompletionState);

    /** 获取通知管理器 */
    static TSharedPtr<SNotificationList> GetNotificationList();
    
    /** 扫描确认通知自动消失回调 */
    static void OnScanConfirmationAutoExpire();
    
    /** 当前确认通知的引用 */
    static TSharedPtr<SNotificationItem> CurrentConfirmationNotification;
    
    /** 扫描确认通知的自动消失定时器 */
    static FTimerHandle ScanConfirmationTimerHandle;
};