// Copyright Epic Games, Inc. All Rights Reserved.

#include "LuaCodeGenerator.h"
#include "EmmyLuaIntelliSense.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/Engine.h"
#include "Misc/DateTime.h"

FString FEmmyLuaCodeGenerator::GenerateBlueprint(const UBlueprint* Blueprint)
{
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        return TEXT("");
    }

    FString Result;
    Result += FString::Printf(TEXT("---@class %s : %s\n"), 
        *Blueprint->GetName(), 
        *GetTypeName(Blueprint->GeneratedClass->GetSuperClass()));
    
    // 添加类注释
    if (!Blueprint->BlueprintDescription.IsEmpty())
    {
        Result += FString::Printf(TEXT("---@comment %s\n"), 
            *EscapeComments(Blueprint->BlueprintDescription));
    }
    
    // 生成类的基本信息
    Result += FString::Printf(TEXT("local %s = {}\n\n"), *Blueprint->GetName());
    
    // 生成属性
    GenerateClassProperties(Blueprint->GeneratedClass, Result);
    
    // 生成函数
    GenerateClassFunctions(Blueprint->GeneratedClass, Result);
    
    // 生成蓝图特有的内容
    GenerateBlueprintSpecific(Blueprint, Result);
    
    Result += FString::Printf(TEXT("\nreturn %s\n"), *Blueprint->GetName());
    
    return Result;
}

FString FEmmyLuaCodeGenerator::GenerateClass(const UClass* Class)
{
    if (!Class)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateClass: Class is null"));
        return TEXT("");
    }
    
    if (!IsValid(Class))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateClass: Class is not valid: %s"), Class ? *Class->GetName() : TEXT("null"));
        return TEXT("");
    }

    FString Result;
    FString ClassName = GetTypeName(Class);
    if (ClassName.IsEmpty() || ClassName == TEXT("Error") || ClassName == TEXT("Invalid"))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateClass: Invalid class name for class: %s"), *Class->GetName());
        return TEXT("");
    }
    
    // 生成类声明，注释放在同一行
    FString ClassComment = Class->GetMetaData(TEXT("Comment"));
    const UClass* SuperClass = Class->GetSuperClass();
    
    if (SuperClass && IsValid(SuperClass))
    {
        FString SuperClassName = GetTypeName(SuperClass);
        if (!SuperClassName.IsEmpty() && SuperClassName != TEXT("Error") && SuperClassName != TEXT("Invalid"))
        {
            if (!ClassComment.IsEmpty())
            {
                Result += FString::Printf(TEXT("---@class %s : %s @%s\n"), 
                    *ClassName, 
                    *SuperClassName,
                    *EscapeComments(ClassComment));
            }
            else
            {
                Result += FString::Printf(TEXT("---@class %s : %s\n"), 
                    *ClassName, 
                    *SuperClassName);
            }
        }
        else
        {
            UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateClass: Invalid super class name for %s"), *Class->GetName());
            if (!ClassComment.IsEmpty())
            {
                Result += FString::Printf(TEXT("---@class %s @%s\n"), *ClassName, *EscapeComments(ClassComment));
            }
            else
            {
                Result += FString::Printf(TEXT("---@class %s\n"), *ClassName);
            }
        }
    }
    else
    {
        if (!ClassComment.IsEmpty())
        {
            Result += FString::Printf(TEXT("---@class %s @%s\n"), *ClassName, *EscapeComments(ClassComment));
        }
        else
        {
            Result += FString::Printf(TEXT("---@class %s\n"), *ClassName);
        }
    }
    
    // 生成属性（紧跟在类声明后面）
    GenerateClassProperties(Class, Result);
    
    Result += FString::Printf(TEXT("local %s = {}\n\n"), *ClassName);
    
    // 生成函数
    GenerateClassFunctions(Class, Result);
    
    Result += FString::Printf(TEXT("\nreturn %s\n"), *ClassName);
    
    return Result;
}

FString FEmmyLuaCodeGenerator::GenerateStruct(const UScriptStruct* Struct)
{
    if (!Struct)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateStruct: Struct is null"));
        return TEXT("");
    }
    
    if (!IsValid(Struct))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateStruct: Struct is not valid: %s"), Struct ? *Struct->GetName() : TEXT("null"));
        return TEXT("");
    }

    FString Result;
    FString StructName = GetTypeName(Struct);
    if (StructName.IsEmpty() || StructName == TEXT("Error") || StructName == TEXT("Invalid"))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateStruct: Invalid struct name for struct: %s"), *Struct->GetName());
        return TEXT("");
    }
    
    // 生成结构体声明，注释放在同一行
    FString StructComment = Struct->GetMetaData(TEXT("Comment"));
    if (!StructComment.IsEmpty())
    {
        Result += FString::Printf(TEXT("---@class %s @%s\n"), *StructName, *EscapeComments(StructComment));
    }
    else
    {
        Result += FString::Printf(TEXT("---@class %s\n"), *StructName);
    }
    
    // 生成属性（紧跟在类声明后面）
    for (TFieldIterator<FProperty> PropertyIt(Struct); PropertyIt; ++PropertyIt)
    {
        FProperty* Property = *PropertyIt;
        if (ShouldSkipProperty(Property))
        {
            continue;
        }
        
        GenerateProperty(Property, Result);
    }
    
    Result += FString::Printf(TEXT("local %s = {}\n\n"), *StructName);
    
    Result += FString::Printf(TEXT("\nreturn %s\n"), *StructName);
    
    return Result;
}

FString FEmmyLuaCodeGenerator::GenerateEnum(const UEnum* Enum)
{
    if (!Enum)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateEnum: Enum is null"));
        return TEXT("");
    }
    
    if (!IsValid(Enum))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateEnum: Enum is not valid: %s"), Enum ? *Enum->GetName() : TEXT("null"));
        return TEXT("");
    }

    FString Result;
    FString EnumName = GetTypeName(Enum);
    if (EnumName.IsEmpty() || EnumName == TEXT("Error") || EnumName == TEXT("Invalid"))
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("GenerateEnum: Invalid enum name for enum: %s"), *Enum->GetName());
        return TEXT("");
    }
    
    // 添加枚举注释
    FString EnumComment = Enum->GetMetaData(TEXT("Comment"));
    if (!EnumComment.IsEmpty())
    {
        Result += FString::Printf(TEXT("---%s\n"), *EscapeComments(EnumComment));
    }
    
    // 生成枚举类声明
    Result += FString::Printf(TEXT("---@class %s\n"), *EnumName);
    
    // 生成枚举值字段声明
    for (int32 i = 0; i < Enum->NumEnums() - 1; ++i) // -1 to skip _MAX
    {
        FString EnumValueName = Enum->GetNameStringByIndex(i);
        int64 EnumValue = Enum->GetValueByIndex(i);
        
        Result += FString::Printf(TEXT("---@field %s integer\n"), 
            *EscapeSymbolName(EnumValueName));
    }
    
    // 生成枚举表定义
    Result += FString::Printf(TEXT("local %s = {}\n\n"), *EnumName);
    
    Result += FString::Printf(TEXT("return %s\n"), *EnumName);
    
    return Result;
}

FString FEmmyLuaCodeGenerator::GenerateUETypes(const TArray<const UField*>& Types)
{
    FString Result;
    
    Result += TEXT("-- Generated UE4 Types for Lua\n");
    Result += FString::Printf(TEXT("-- Generated at: %s\n\n"), *FDateTime::Now().ToString());
    
    for (const UField* Type : Types)
    {
        if (const UClass* Class = Cast<UClass>(Type))
        {
            Result += GenerateClass(Class) + TEXT("\n");
        }
        else if (const UScriptStruct* Struct = Cast<UScriptStruct>(Type))
        {
            Result += GenerateStruct(Struct) + TEXT("\n");
        }
        else if (const UEnum* Enum = Cast<UEnum>(Type))
        {
            Result += GenerateEnum(Enum) + TEXT("\n");
        }
    }
    
    return Result;
}

FString FEmmyLuaCodeGenerator::GenerateUETable(const TArray<const UField*>& Types)
{
    FString Content = TEXT("---@class UE\r\n");
    
    for (const UField* Type : Types)
    {
        if (!Type->IsNative())
            continue;
            
        const FString Name = GetTypeName(Type);
        Content += FString::Printf(TEXT("---@field %s %s\r\n"), *Name, *Name);
    }
    
    Content += TEXT("\r\n");
    return Content;
}

void FEmmyLuaCodeGenerator::GenerateClassProperties(const UClass* Class, FString& Code)
{
    for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
    {
        FProperty* Property = *PropertyIt;
        if (ShouldSkipProperty(Property))
        {
            continue;
        }
        
        GenerateProperty(Property, Code);
    }
}

void FEmmyLuaCodeGenerator::GenerateClassFunctions(const UClass* Class, FString& Code)
{
    FString ClassName = GetTypeName(Class);
    
    for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
    {
        UFunction* Function = *FunctionIt;
        if (ShouldSkipFunction(Function))
        {
            continue;
        }
        
        GenerateFunction(Function, Code, ClassName);
    }
}

void FEmmyLuaCodeGenerator::GenerateBlueprintSpecific(const UBlueprint* Blueprint, FString& Code)
{
    // 这里可以添加蓝图特有的生成逻辑
    // 比如蓝图变量、蓝图函数等
}

void FEmmyLuaCodeGenerator::GenerateProperty(const FProperty* Property, FString& Code)
{
    if (!Property)
    {
        return;
    }
    
    FString PropertyType = GetPropertyType(Property);
    FString PropertyName = EscapeSymbolName(Property->GetName());
    
    // 生成属性声明，注释放在同一行
    FString PropertyComment = Property->GetMetaData(TEXT("Comment"));
    if (!PropertyComment.IsEmpty())
    {
        Code += FString::Printf(TEXT("---@field %s %s @%s\n"), *PropertyName, *PropertyType, *EscapeComments(PropertyComment));
    }
    else
    {
        Code += FString::Printf(TEXT("---@field %s %s\n"), *PropertyName, *PropertyType);
    }
}

void FEmmyLuaCodeGenerator::GenerateFunction(const UFunction* Function, FString& Code, const FString& ClassName)
{
    if (!Function)
    {
        return;
    }
    
    FString FunctionName = EscapeSymbolName(Function->GetName());
    
    // 添加函数注释
    FString FunctionComment = Function->GetMetaData(TEXT("Comment"));
    if (!FunctionComment.IsEmpty())
    {
        Code += FString::Printf(TEXT("---%s\n"), *EscapeComments(FunctionComment));
    }
    
    // 生成参数
    TArray<FString> Parameters;
    FString ReturnType = TEXT("void");
    
    for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
    {
        FProperty* Param = *ParamIt;
        
        if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            ReturnType = GetPropertyType(Param);
        }
        else if (!Param->HasAnyPropertyFlags(CPF_OutParm))
        {
            FString ParamType = GetPropertyType(Param);
            FString ParamName = EscapeSymbolName(Param->GetName());
            
            Code += FString::Printf(TEXT("---@param %s %s\n"), *ParamName, *ParamType);
            Parameters.Add(ParamName);
        }
    }
    
    // 生成返回类型
    if (ReturnType != TEXT("void"))
    {
        Code += FString::Printf(TEXT("---@return %s\n"), *ReturnType);
    }
    
    // 生成函数声明，根据是否为静态函数选择连接符
    FString ParamList = FString::Join(Parameters, TEXT(", "));
    bool bIsStatic = Function->HasAnyFunctionFlags(FUNC_Static);
    FString Connector = bIsStatic ? TEXT(".") : TEXT(":");
    
    if (!ClassName.IsEmpty())
    {
        Code += FString::Printf(TEXT("function %s%s%s(%s) end\n\n"), *ClassName, *Connector, *FunctionName, *ParamList);
    }
    else
    {
        Code += FString::Printf(TEXT("function %s(%s) end\n\n"), *FunctionName, *ParamList);
    }
}

FString FEmmyLuaCodeGenerator::GetPropertyType(const FProperty* Property)
{
    if (!Property)
    {
        return TEXT("any");
    }
    
    return GetTypeName(Property);
}

FString FEmmyLuaCodeGenerator::GetTypeName(const UField* Field)
{
    if (!Field)
    {
        return TEXT("");
    }
    
    // 完全匹配UnLua的GetTypeName实现
    FString FieldName = Field->GetName();
    if (!Field->IsNative() && FieldName.EndsWith(TEXT("_C")))
    {
        return FieldName;
    }
    
    const UStruct* Struct = Cast<UStruct>(Field);
    if (Struct)
    {
        return Struct->GetPrefixCPP() + Struct->GetName();
    }
    
    return FieldName;
}

FString FEmmyLuaCodeGenerator::GetTypeName(const UObject* Object)
{
    if (!Object)
    {
        return TEXT("");
    }
    
    // 完全匹配UnLua的GetTypeName(UObject*)实现
    FString ObjectName = Object->GetName();
    if (!Object->IsNative() && ObjectName.EndsWith(TEXT("_C")))
    {
        return ObjectName;
    }
    
    const UStruct* Struct = Cast<UStruct>(Object);
    if (Struct)
    {
        return Struct->GetPrefixCPP() + Struct->GetName();
    }
    
    return ObjectName;
}

FString FEmmyLuaCodeGenerator::GetTypeName(const FProperty* Property)
{
    if (!Property)
        return "any";

    if (CastField<FByteProperty>(Property))
        return "integer";

    if (CastField<FInt8Property>(Property))
        return "integer";

    if (CastField<FInt16Property>(Property))
        return "integer";

    if (CastField<FIntProperty>(Property))
        return "integer";

    if (CastField<FInt64Property>(Property))
        return "integer";

    if (CastField<FUInt16Property>(Property))
        return "integer";

    if (CastField<FUInt32Property>(Property))
        return "integer";

    if (CastField<FUInt64Property>(Property))
        return "integer";

    if (CastField<FFloatProperty>(Property))
        return "number";

    if (CastField<FDoubleProperty>(Property))
        return "number";

    if (CastField<FEnumProperty>(Property))
        return ((FEnumProperty*)Property)->GetEnum()->GetName();

    if (CastField<FBoolProperty>(Property))
        return TEXT("boolean");

    if (CastField<FClassProperty>(Property))
    {
        const UClass* Class = ((FClassProperty*)Property)->MetaClass;
        return FString::Printf(TEXT("TSubclassOf<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FSoftObjectProperty>(Property))
    {
        if (((FSoftObjectProperty*)Property)->PropertyClass->IsChildOf(UClass::StaticClass()))
        {
            const UClass* Class = ((FSoftClassProperty*)Property)->MetaClass;
            return FString::Printf(TEXT("TSoftClassPtr<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
        }
        const UClass* Class = ((FSoftObjectProperty*)Property)->PropertyClass;
        return FString::Printf(TEXT("TSoftObjectPtr<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FObjectProperty>(Property))
    {
        const UClass* Class = ((FObjectProperty*)Property)->PropertyClass;
        if (Cast<UBlueprintGeneratedClass>(Class))
        {
            return FString::Printf(TEXT("%s"), *Class->GetName());
        }
        return FString::Printf(TEXT("%s%s"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FWeakObjectProperty>(Property))
    {
        const UClass* Class = ((FWeakObjectProperty*)Property)->PropertyClass;
        return FString::Printf(TEXT("TWeakObjectPtr<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FLazyObjectProperty>(Property))
    {
        const UClass* Class = ((FLazyObjectProperty*)Property)->PropertyClass;
        return FString::Printf(TEXT("TLazyObjectPtr<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FInterfaceProperty>(Property))
    {
        const UClass* Class = ((FInterfaceProperty*)Property)->InterfaceClass;
        return FString::Printf(TEXT("TScriptInterface<%s%s>"), Class->GetPrefixCPP(), *Class->GetName());
    }

    if (CastField<FNameProperty>(Property))
        return "string";

    if (CastField<FStrProperty>(Property))
        return "string";

    if (CastField<FTextProperty>(Property))
        return "string";

    if (CastField<FArrayProperty>(Property))
    {
        const FProperty* Inner = ((FArrayProperty*)Property)->Inner;
        return FString::Printf(TEXT("TArray<%s>"), *GetTypeName(Inner));
    }

    if (CastField<FMapProperty>(Property))
    {
        const FProperty* KeyProp = ((FMapProperty*)Property)->KeyProp;
        const FProperty* ValueProp = ((FMapProperty*)Property)->ValueProp;
        return FString::Printf(TEXT("TMap<%s, %s>"), *GetTypeName(KeyProp), *GetTypeName(ValueProp));
    }

    if (CastField<FSetProperty>(Property))
    {
        const FProperty* ElementProp = ((FSetProperty*)Property)->ElementProp;
        return FString::Printf(TEXT("TSet<%s>"), *GetTypeName(ElementProp));
    }

    if (CastField<FStructProperty>(Property))
        return ((FStructProperty*)Property)->Struct->GetStructCPPName();

    FString PropertyTypeName = Property->GetCPPType();
    if (!PropertyTypeName.IsEmpty())
    {
        return PropertyTypeName;
    }

    return "any";
}

FString FEmmyLuaCodeGenerator::EscapeComments(const FString& Comment)
{
    FString Result = Comment;
    
    // 移除C++风格的注释符号
    Result = Result.Replace(TEXT("/**"), TEXT(""));
    Result = Result.Replace(TEXT("*/"), TEXT(""));
    Result = Result.Replace(TEXT("/*"), TEXT(""));
    Result = Result.Replace(TEXT("//"), TEXT(""));
    Result = Result.Replace(TEXT("*"), TEXT(""));
    
    // 转义注释中的特殊字符
    Result = Result.Replace(TEXT("\n"), TEXT(" "));
    Result = Result.Replace(TEXT("\r"), TEXT(""));
    Result = Result.Replace(TEXT("\t"), TEXT(" "));
    
    // 移除多余的空格
    while (Result.Contains(TEXT("  ")))
    {
        Result = Result.Replace(TEXT("  "), TEXT(" "));
    }
    
    return Result.TrimStartAndEnd();
}

FString FEmmyLuaCodeGenerator::EscapeSymbolName(const FString& Name)
{
    FString Result = Name;
    
    // Lua关键字列表
    static const TSet<FString> LuaKeywords = {
        TEXT("and"), TEXT("break"), TEXT("do"), TEXT("else"), TEXT("elseif"),
        TEXT("end"), TEXT("false"), TEXT("for"), TEXT("function"), TEXT("if"),
        TEXT("in"), TEXT("local"), TEXT("nil"), TEXT("not"), TEXT("or"),
        TEXT("repeat"), TEXT("return"), TEXT("then"), TEXT("true"), TEXT("until"), TEXT("while")
    };
    
    // 如果是Lua关键字，添加下划线前缀
    if (LuaKeywords.Contains(Result.ToLower()))
    {
        Result = TEXT("_") + Result;
    }
    
    // 替换无效字符
    Result = Result.Replace(TEXT(" "), TEXT("_"));
    Result = Result.Replace(TEXT("-"), TEXT("_"));
    Result = Result.Replace(TEXT("."), TEXT("_"));
    
    // 确保以字母或下划线开头
    if (Result.Len() > 0 && !FChar::IsAlpha(Result[0]) && Result[0] != TEXT('_'))
    {
        Result = TEXT("_") + Result;
    }
    
    return Result;
}

bool FEmmyLuaCodeGenerator::ShouldSkipType(const UField* Field)
{
    if (!Field || !IsValid(Field))
    {
        return true;
    }
    
    // 获取字段名称，添加异常处理
    FString FieldName;
    try
    {
        FieldName = Field->GetName();
    }
    catch (...)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("ShouldSkipType: Exception getting field name"));
        return true;
    }
    
    // 跳过名称无效的类型
    if (FieldName.IsEmpty() || FieldName == TEXT("None") || FieldName == TEXT("NULL"))
    {
        return true;
    }
    
    // 跳过已弃用的类型
    try
    {
        if (Field->HasMetaData(TEXT("Deprecated")))
        {
            return true;
        }
    }
    catch (...)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("ShouldSkipType: Exception checking deprecated metadata for %s"), *FieldName);
    }
    
    // 跳过编辑器专用类型
    if (FieldName.Contains(TEXT("Editor")))
    {
        return true;
    }
    
    // 跳过内部类型
    if (FieldName.StartsWith(TEXT("_")))
    {
        return true;
    }
    
    return false;
}

bool FEmmyLuaCodeGenerator::ShouldSkipProperty(const FProperty* Property)
{
    if (!Property)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("ShouldSkipProperty: Property is null"));
        return true;
    }
    
    // 跳过非public属性（私有和受保护属性）
    if (Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPrivate | CPF_NativeAccessSpecifierProtected))
    {
        return true;
    }
    
    // 跳过已弃用的属性
    if (Property->HasMetaData(TEXT("Deprecated")))
    {
        return true;
    }
    
    // 跳过编辑器专用属性
    if (Property->HasAnyPropertyFlags(CPF_EditorOnly))
    {
        return true;
    }
    
    return false;
}

bool FEmmyLuaCodeGenerator::ShouldSkipFunction(const UFunction* Function)
{
    if (!Function)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("ShouldSkipFunction: Function is null"));
        return true;
    }
    
    // 跳过私有函数
    if (Function->HasAnyFunctionFlags(FUNC_Private))
    {
        return true;
    }
    
    // 跳过已弃用的函数
    if (Function->HasMetaData(TEXT("Deprecated")))
    {
        return true;
    }
    
    // 跳过编辑器专用函数
    if (Function->HasAnyFunctionFlags(FUNC_EditorOnly))
    {
        return true;
    }
    
    // 跳过事件函数（通常以Event开头）
    if (Function->GetName().StartsWith(TEXT("Event")))
    {
        return true;
    }
    
    // 跳过委托函数
    if (Function->HasAnyFunctionFlags(FUNC_Delegate | FUNC_MulticastDelegate))
    {
        return true;
    }
    
    return false;
}

bool FEmmyLuaCodeGenerator::IsValidFunction(const UFunction* Function)
{
    if (!Function)
    {
        UE_LOG(LogEmmyLuaIntelliSense, Warning, TEXT("IsValidFunction: Function is null"));
        return false;
    }
    return !ShouldSkipFunction(Function);
}

bool FEmmyLuaCodeGenerator::IsValidFunctionName(const FString& Name)
{
    if (Name.IsEmpty())
    {
        return false;
    }
    
    // 检查是否以字母或下划线开头
    if (!FChar::IsAlpha(Name[0]) && Name[0] != TEXT('_'))
    {
        return false;
    }
    
    // 检查是否只包含字母、数字和下划线
    for (int32 i = 1; i < Name.Len(); ++i)
    {
        if (!FChar::IsAlnum(Name[i]) && Name[i] != TEXT('_'))
        {
            return false;
        }
    }
    
    return true;
}