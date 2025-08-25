// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"

#ifndef LUACODEGENERATOR_H_INCLUDED
#define LUACODEGENERATOR_H_INCLUDED

// 确保FLuaCodeGenerator不被误认为是模板
#ifdef FLuaCodeGenerator
#undef FLuaCodeGenerator
#endif

/**
 * Lua代码生成器
 * 负责将UE反射信息转换为Lua代码
 */
class EMMYLUAINTELLISENSE_API FEmmyLuaCodeGenerator
{
public:
    /** 生成蓝图的Lua代码 */
    static FString GenerateBlueprint(const UBlueprint* Blueprint);

    /** 生成类的Lua代码 */
    static FString GenerateClass(const UClass* Class);

    /** 生成结构体的Lua代码 */
    static FString GenerateStruct(const UScriptStruct* Struct);

    /** 生成枚举的Lua代码 */
    static FString GenerateEnum(const UEnum* Enum);

    /** 生成函数的Lua代码 */
    static FString GenerateFunction(const UFunction* Function, const FString& ClassName = "");

    /** 生成属性的Lua代码 */
    static FString GenerateProperty(const FProperty* Property);

    /** 生成UE核心类型的Lua代码 */
    static FString GenerateUETypes(const TArray<const UField*>& Types);
    
    /** 生成UnLua格式的UE表 */
    static FString GenerateUETable(const TArray<const UField*>& Types);

    /** 获取类型名称 */
    static FString GetTypeName(const UObject* Object);
    
    /** 获取字段类型名称 */
    static FString GetTypeName(const UField* Field);

    /** 获取属性类型名称 */
    static FString GetPropertyType(const FProperty* Property);

    /** 转义注释内容 */
    static FString EscapeComments(const FString& Comment);
    static FString EscapeComments(const FString& Comment, bool bInline);

    /** 转义符号名称 */
    static FString EscapeSymbolName(const FString& Name);

    /** 检查函数是否有效 */
    static bool IsValidFunction(const UFunction* Function);

    /** 检查函数名是否有效 */
    static bool IsValidFunctionName(const FString& Name);

    /** 检查类型是否应该跳过 */
    static bool ShouldSkipType(const UField* Field);
    
    /** 检查函数是否应该跳过 */
    static bool ShouldSkipFunction(const UFunction* Function);

    /** 检查属性是否应该跳过 */
    static bool ShouldSkipProperty(const FProperty* Property);

private:
    /** 生成类属性的Lua代码 */
    static void GenerateClassProperties(const UClass* Class, FString& Code);
    
    /** 生成类函数的Lua代码 */
    static void GenerateClassFunctions(const UClass* Class, FString& Code);
    
    /** 生成蓝图特定的Lua代码 */
    static void GenerateBlueprintSpecific(const UBlueprint* Blueprint, FString& Code);

    /** 生成属性的Lua代码 */
    static void GenerateProperty(const FProperty* Property, FString& Code);

    /** 生成函数的Lua代码 */
    static void GenerateFunction(const UFunction* Function, FString& Code, const FString& ClassName = "");
};

#endif // LUACODEGENERATOR_H_INCLUDED