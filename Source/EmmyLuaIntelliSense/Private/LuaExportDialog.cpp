// Copyright Epic Games, Inc. All Rights Reserved.

#include "LuaExportDialog.h"
#include "LuaExportManager.h"
#include "EmmyLuaIntelliSense.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "EditorStyleSet.h"

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::CurrentConfirmationNotification = nullptr;


void FLuaExportDialog::ShowExportConfirmation()
{
    const FString Message = TEXT("是否要导出Lua IntelliSense文件以获得更好的代码提示？");
    
    FLuaExportNotificationManager::ShowExportConfirmation(Message);
}

void FLuaExportDialog::ShowExportConfirmation(int32 FileCount)
{
    FString Message;
    if (FileCount > 0)
    {
        Message = FString::Printf(TEXT("检测到 %d 个文件需要导出，是否要导出Lua IntelliSense文件以获得更好的代码提示？"), FileCount);
    }
    else
    {
        Message = TEXT("是否要导出Lua IntelliSense文件以获得更好的代码提示？");
    }

    FLuaExportNotificationManager::ShowExportConfirmation(Message);
}

void FLuaExportDialog::ShowExportConfirmation(int32 BlueprintCount, int32 NativeTypeCount)
{
    FString Message;
    if (BlueprintCount > 0 && NativeTypeCount > 0)
    {
        Message = FString::Printf(TEXT("检测到 %d 个蓝图文件和 %d 个原生类型文件需要导出，是否要导出Lua IntelliSense文件以获得更好的代码提示？"), BlueprintCount, NativeTypeCount);
    }
    else if (BlueprintCount > 0)
    {
        Message = FString::Printf(TEXT("检测到 %d 个蓝图文件需要导出，是否要导出Lua IntelliSense文件以获得更好的代码提示？"), BlueprintCount);
    }
    else if (NativeTypeCount > 0)
    {
        Message = FString::Printf(TEXT("检测到 %d 个原生类型文件需要导出，是否要导出Lua IntelliSense文件以获得更好的代码提示？"), NativeTypeCount);
    }
    else
    {
        Message = TEXT("是否要导出Lua IntelliSense文件以获得更好的代码提示？");
    }

    FLuaExportNotificationManager::ShowExportConfirmation(Message);
}

void FLuaExportDialog::ShowExportConfirmation(int32 BlueprintCount, int32 NativeTypeCount, int32 CoreFileCount)
{
    FString Message;
    TArray<FString> Parts;
    
    if (BlueprintCount > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d 个蓝图文件"), BlueprintCount));
    }
    if (NativeTypeCount > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d 个原生类型文件"), NativeTypeCount));
    }
    if (CoreFileCount > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d 个核心文件"), CoreFileCount));
    }
    
    if (Parts.Num() > 0)
    {
        FString FileList;
        for (int32 i = 0; i < Parts.Num(); ++i)
        {
            if (i > 0)
            {
                FileList += TEXT("\n");
            }
            FileList += Parts[i];
        }
        Message = FString::Printf(TEXT("检测到以下文件需要导出：\n%s\n\n是否要导出Lua IntelliSense文件以获得更好的代码提示？"), *FileList);
    }
    else
    {
        Message = TEXT("是否要导出Lua IntelliSense文件以获得更好的代码提示？");
    }

    FLuaExportNotificationManager::ShowExportConfirmation(Message);
}

// FLuaExportNotificationManager Implementation

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowExportConfirmation(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    
    Info.ExpireDuration = 0.0f;
    
    Info.bFireAndForget = false;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = false;
    Info.bUseSuccessFailIcons = false;
    Info.FadeOutDuration = 1.0f;
    
    FNotificationButtonInfo Button1(
        FText::FromString(TEXT("导出")),
        FText::FromString(TEXT("开始导出Lua IntelliSense文件")),
        FSimpleDelegate::CreateStatic(&FLuaExportNotificationManager::OnExportConfirmed) 
    );
    
    FNotificationButtonInfo Button2(
        FText::FromString(TEXT("跳过")), // 按钮文本
        FText::FromString(TEXT("跳过此次导出")), // 工具提示
        FSimpleDelegate::CreateStatic(&FLuaExportNotificationManager::OnExportSkipped) // 回调
    );
    
    Info.ButtonDetails.Add(Button1);
    Info.ButtonDetails.Add(Button2);

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
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("User confirmed Lua export via notification."));
    
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
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("User skipped Lua export via notification."));

    if (CurrentConfirmationNotification.IsValid())
    {
        CurrentConfirmationNotification->SetCompletionState(SNotificationItem::CS_None);
        CurrentConfirmationNotification->ExpireAndFadeout();
        CurrentConfirmationNotification.Reset();
    }
}

TSharedPtr<SNotificationItem> FLuaExportNotificationManager::ShowScanProgress(const FString& Message)
{
    FNotificationInfo Info(FText::FromString(Message));
    Info.bFireAndForget = false;
    Info.bUseLargeFont = false;
    Info.bUseThrobber = true;
    Info.bUseSuccessFailIcons = false;
    Info.FadeOutDuration = 1.0f;
    Info.ExpireDuration = 0.0f; // 不自动过期
    
    // 添加取消按钮
    Info.ButtonDetails.Add(FNotificationButtonInfo(
        FText::FromString(TEXT("取消")),
        FText::FromString(TEXT("取消当前扫描操作")),
        FSimpleDelegate::CreateStatic(&FLuaExportNotificationManager::OnScanCancelled),
        SNotificationItem::CS_Pending
    ));

    return FSlateNotificationManager::Get().AddNotification(Info);
}

void FLuaExportNotificationManager::UpdateScanProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, float Progress)
{
    if (Notification.IsValid())
    {
        Notification->SetText(FText::FromString(Message));
        Notification->SetCompletionState(Progress < 1.0f ? SNotificationItem::CS_Pending : SNotificationItem::CS_Success);
    }
}

void FLuaExportNotificationManager::CompleteScanProgressNotification(TSharedPtr<SNotificationItem> Notification, const FString& Message, bool bSuccess)
{
    if (Notification.IsValid())
    {
        Notification->SetText(FText::FromString(Message));
        Notification->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        Notification->SetFadeOutDuration(3.0f);
        Notification->ExpireAndFadeout();
    }
}

void FLuaExportNotificationManager::OnScanCancelled()
{
    UE_LOG(LogEmmyLuaIntelliSense, Log, TEXT("User cancelled asset scanning."));
    
    // 通知导出管理器取消扫描
    if (ULuaExportManager* ExportManager = GEditor->GetEditorSubsystem<ULuaExportManager>())
    {
        ExportManager->CancelAsyncScan();
    }
}