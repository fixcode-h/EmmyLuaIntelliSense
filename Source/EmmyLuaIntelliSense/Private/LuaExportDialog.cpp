// Copyright Epic Games, Inc. All Rights Reserved.

#include "LuaExportDialog.h"
#include "LuaExportManager.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "EditorStyleSet.h"

// 静态变量定义
TSharedPtr<SNotificationItem> FLuaExportNotificationManager::CurrentConfirmationNotification = nullptr;

// 使用右下角非阻塞通知显示导出确认
void FLuaExportDialog::ShowExportConfirmation()
{
    const FString Message = TEXT("是否要导出Lua IntelliSense文件以获得更好的代码提示？");
    
    // 使用非阻塞通知替代阻塞对话框
    FLuaExportNotificationManager::ShowExportConfirmation(Message);
}

// FLuaExportNotificationManager Implementation

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowExportConfirmation(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    
    // 核心改动：将 ExpireDuration 设置为 0，使其成为一个持久性、可交互的通知
    Info.ExpireDuration = 0.0f;
    
    Info.bFireAndForget = false;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = false;
    Info.bUseSuccessFailIcons = false;
    Info.FadeOutDuration = 1.0f;
    
    // 创建第一个按钮 - 导出
    FNotificationButtonInfo Button1(
        FText::FromString(TEXT("导出")), // 按钮文本
        FText::FromString(TEXT("开始导出Lua IntelliSense文件")), // 工具提示
        FSimpleDelegate::CreateStatic(&FLuaExportNotificationManager::OnExportConfirmed) // 回调
    );
    
    // 创建第二个按钮 - 跳过
    FNotificationButtonInfo Button2(
        FText::FromString(TEXT("跳过")), // 按钮文本
        FText::FromString(TEXT("跳过此次导出")), // 工具提示
        FSimpleDelegate::CreateStatic(&FLuaExportNotificationManager::OnExportSkipped) // 回调
    );
    
    // 将按钮添加到通知信息中
    Info.ButtonDetails.Add(Button1);
    Info.ButtonDetails.Add(Button2);

    // 注意：这里需要用 TWeakPtr 来存储，以避免循环引用
    // 你的类成员 CurrentConfirmationNotification 应该是 TWeakPtr<SNotificationItem>
    TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
    CurrentConfirmationNotification = NotificationPtr; // 假设 CurrentConfirmationNotification 是 TWeakPtr
    CurrentConfirmationNotification->SetCompletionState(SNotificationItem::CS_Pending);
    return NotificationPtr;
}

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowExportSuccess(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    Info.bFireAndForget = true;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = false;
    Info.bUseSuccessFailIcons = true;
    Info.FadeOutDuration = 3.0f;
    Info.ExpireDuration = 5.0f;
    Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));

    return FSlateNotificationManager::Get().AddNotification(Info);
}

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowExportFailure(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    Info.bFireAndForget = true;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = false;
    Info.bUseSuccessFailIcons = true;
    Info.FadeOutDuration = 5.0f;
    Info.ExpireDuration = 10.0f;
    Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.FailImage"));

    return FSlateNotificationManager::Get().AddNotification(Info);
}

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowExportProgress(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    Info.bFireAndForget = false;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = true;
    Info.bUseSuccessFailIcons = false;
    Info.FadeOutDuration = 1.0f;
    Info.ExpireDuration = 1.0f;

    return FSlateNotificationManager::Get().AddNotification(Info);
}

void FLuaExportNotificationManager::UpdateProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, float Progress)
{
    if (Notification.IsValid())
    {
        Notification->SetText(FText::FromString(Message));
        Notification->SetCompletionState(Progress < 1.0f ? SNotificationItem::CS_Pending : SNotificationItem::CS_Success);
    }
}

void FLuaExportNotificationManager::CompleteProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, bool bSuccess)
{
    if (Notification.IsValid())
    {
        Notification->SetText(FText::FromString(Message));
        Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        Notification->SetFadeOutDuration(3.0f);
        Notification->ExpireAndFadeout();
    }
}

void FLuaExportNotificationManager::OnExportConfirmed()
{
    UE_LOG(LogTemp, Log, TEXT("User confirmed Lua export via notification."));
    
    // 关闭确认通知
    if (CurrentConfirmationNotification.IsValid())
    {
        CurrentConfirmationNotification->SetCompletionState(SNotificationItem::CS_Success);
        CurrentConfirmationNotification->ExpireAndFadeout();
        CurrentConfirmationNotification.Reset();
    }
    
    if (ULuaExportManager* ExportManager = ULuaExportManager::Get())
    {
        ExportManager->ExportIncremental();
    }
}

void FLuaExportNotificationManager::OnExportSkipped()
{
    UE_LOG(LogTemp, Log, TEXT("User skipped Lua export via notification."));
    
    // 关闭确认通知
    if (CurrentConfirmationNotification.IsValid())
    {
        CurrentConfirmationNotification->SetCompletionState(SNotificationItem::CS_None);
        CurrentConfirmationNotification->ExpireAndFadeout();
        CurrentConfirmationNotification.Reset();
    }
}