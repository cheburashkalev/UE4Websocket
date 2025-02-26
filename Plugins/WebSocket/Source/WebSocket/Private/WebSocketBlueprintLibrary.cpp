/*
* uewebsocket - unreal engine 4 websocket plugin
*
* Copyright (C) 2017 feiwu <feixuwu@outlook.com>
*
*  This library is free software; you can redistribute it and/or
*  modify it under the terms of the GNU Lesser General Public
*  License as published by the Free Software Foundation:
*  version 2.1 of the License.
*
*  This library is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*  Lesser General Public License for more details.
*
*  You should have received a copy of the GNU Lesser General Public
*  License along with this library; if not, write to the Free Software
*  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
*  MA  02110-1301  USA
*/

#include "WebSocket.h"
#include "WebSocketContext.h"
#include "WebSocketBlueprintLibrary.h"
#include "Runtime/Launch/Resources/Version.h"



UWebSocketContext* s_websocketCtx;

static bool GetTextFromObject(const TSharedRef<FJsonObject>& Obj, FText& TextOut)
{
	// get the prioritized culture name list
	FCultureRef CurrentCulture = FInternationalization::Get().GetCurrentCulture();
	TArray<FString> CultureList = CurrentCulture->GetPrioritizedParentCultureNames();

	// try to follow the fall back chain that the engine uses
	FString TextString;
	for (const FString& CultureCode : CultureList)
	{
		if (Obj->TryGetStringField(CultureCode, TextString))
		{
			TextOut = FText::FromString(TextString);
			return true;
		}
	}

	// no luck, is this possibly an unrelated json object?
	return false;
}



UWebSocketBase* UWebSocketBlueprintLibrary::Connect(const FString& url, bool& connectFail)
{
	if (s_websocketCtx == nullptr)
	{
		s_websocketCtx =  NewObject<UWebSocketContext>();
		s_websocketCtx->CreateCtx();
		s_websocketCtx->AddToRoot();
	}

	return s_websocketCtx->Connect(url, TMap<FString, FString>(), connectFail);
}

UWebSocketBase* UWebSocketBlueprintLibrary::ConnectWithHeader(const FString& url, const TArray<FWebSocketHeaderPair>& header, bool& connectFail)
{
	if (s_websocketCtx == nullptr)
	{
		s_websocketCtx = NewObject<UWebSocketContext>();
		s_websocketCtx->CreateCtx();
		s_websocketCtx->AddToRoot();
	}

	TMap<FString, FString> headerMap;
	for (int i = 0; i < header.Num(); i++)
	{
		headerMap.Add(header[i].key, header[i].value);
	}

	return s_websocketCtx->Connect(url, headerMap, connectFail);
}

bool UWebSocketBlueprintLibrary::GetJsonIntField(const FString& data, const FString& key, int& iValue)
{
	FString tmpData = data;
	TSharedRef<TJsonReader<TCHAR>> Reader = FJsonStringReader::Create(MoveTemp(tmpData));

	TSharedPtr<FJsonObject> JsonObject;
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return false;
	}

	if (!JsonObject->HasField(key))
	{
		return false;
	}

	iValue = JsonObject->GetIntegerField(key);

	return true;
}

bool UWebSocketBlueprintLibrary::ObjectToJson(UObject* Object, FString& data)
{
	UClass* ClassObject = Object->GetClass();
	TSharedRef<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
	if (!UObjectToJsonObject(ClassObject, Object, JsonObject, 0, 0) )
	{
		return false;
	}

	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR> > > JsonWriter = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR> >::Create(&data, 0);
	bool bSuccess = FJsonSerializer::Serialize(JsonObject, JsonWriter);
	JsonWriter->Close();

	return bSuccess;
}

UObject* UWebSocketBlueprintLibrary::JsonToObject(const FString& data, UClass * ClassObject, bool checkAll)
{
	UObject* pNewObject = NewObject<UObject>((UObject*)GetTransientPackage(), ClassObject);

	FString tmpData = data;
	TSharedRef<TJsonReader<TCHAR>> Reader = FJsonStringReader::Create(MoveTemp(tmpData));

	TSharedPtr<FJsonObject> JsonObject;
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}

	if (JsonObjectToUStruct(JsonObject.ToSharedRef(), ClassObject, pNewObject, 0, 0))
	{
		return pNewObject;
	}

	return nullptr;
}


bool UWebSocketBlueprintLibrary::JsonValueToUProperty(TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue, int64 CheckFlags, int64 SkipFlags)
{
	if (!JsonValue.IsValid())
	{
		UE_LOG(WebSocket, Error, TEXT("JsonValueToUProperty - Invalid value JSON key"));
		return false;
	}

	bool bArrayProperty = Property->IsA<UArrayProperty>();
	bool bJsonArray = JsonValue->Type == EJson::Array;

	if (!bJsonArray)
	{
		if (bArrayProperty)
		{
			UE_LOG(WebSocket, Error, TEXT("JsonValueToUProperty - Attempted to import TArray from non-array JSON key"));
			return false;
		}

		if (Property->ArrayDim != 1)
		{
			UE_LOG(WebSocket, Warning, TEXT("Ignoring excess properties when deserializing %s"), *Property->GetName());
		}

		return ConvertScalarJsonValueToUProperty(JsonValue, Property, OutValue, CheckFlags, SkipFlags);
	}

	// In practice, the ArrayDim == 1 check ought to be redundant, since nested arrays of UPropertys are not supported
	if (bArrayProperty && Property->ArrayDim == 1)
	{
		// Read into TArray
		return ConvertScalarJsonValueToUProperty(JsonValue, Property, OutValue, CheckFlags, SkipFlags);
	}

	// We're deserializing a JSON array
	const auto& ArrayValue = JsonValue->AsArray();
	if (Property->ArrayDim < ArrayValue.Num())
	{
		UE_LOG(WebSocket, Warning, TEXT("Ignoring excess properties when deserializing %s"), *Property->GetName());
	}

	// Read into native array
	int ItemsToRead = FMath::Clamp(ArrayValue.Num(), 0, Property->ArrayDim);
	for (int Index = 0; Index != ItemsToRead; ++Index)
	{
		if (!ConvertScalarJsonValueToUProperty(ArrayValue[Index], Property, (char*)OutValue + Index * Property->ElementSize, CheckFlags, SkipFlags))
		{
			return false;
		}
	}
	return true;
}

extern bool GetTextFromObject(const TSharedRef<FJsonObject>& Obj, FText& TextOut);

bool UWebSocketBlueprintLibrary::ConvertScalarJsonValueToUProperty(TSharedPtr<FJsonValue> JsonValue, UProperty* Property, void* OutValue, int64 CheckFlags, int64 SkipFlags)
{
	if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
	{
		if (JsonValue->Type == EJson::String)
		{
			// see if we were passed a string for the enum
			const UEnum* Enum = EnumProperty->GetEnum();
			check(Enum);
			FString StrValue = JsonValue->AsString();
			int64 IntValue = Enum->GetValueByName(FName(*StrValue));
			if (IntValue == INDEX_NONE)
			{
				//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable import enum %s from string value %s for property %s"), *Enum->CppType, *StrValue, *Property->GetNameCPP());
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, IntValue);
		}
		else
		{
			// AsNumber will log an error for completely inappropriate types (then give us a default)
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, (int64)JsonValue->AsNumber());
		}
	}
	else if (UNumericProperty *NumericProperty = Cast<UNumericProperty>(Property))
	{
		if (NumericProperty->IsEnum() && JsonValue->Type == EJson::String)
		{
			// see if we were passed a string for the enum
			const UEnum* Enum = NumericProperty->GetIntPropertyEnum();
			check(Enum); // should be assured by IsEnum()
			FString StrValue = JsonValue->AsString();
			int64 IntValue = Enum->GetValueByName(FName(*StrValue));
			if (IntValue == INDEX_NONE)
			{
				//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable import enum %s from string value %s for property %s"), *Enum->CppType, *StrValue, *Property->GetNameCPP());
				return false;
			}
			NumericProperty->SetIntPropertyValue(OutValue, IntValue);
		}
		else if (NumericProperty->IsFloatingPoint())
		{
			// AsNumber will log an error for completely inappropriate types (then give us a default)
			NumericProperty->SetFloatingPointPropertyValue(OutValue, JsonValue->AsNumber());
		}
		else if (NumericProperty->IsInteger())
		{
			if (JsonValue->Type == EJson::String)
			{
				// parse string -> int64 ourselves so we don't lose any precision going through AsNumber (aka double)
				NumericProperty->SetIntPropertyValue(OutValue, FCString::Atoi64(*JsonValue->AsString()));
			}
			else
			{
				// AsNumber will log an error for completely inappropriate types (then give us a default)
				NumericProperty->SetIntPropertyValue(OutValue, (int64)JsonValue->AsNumber());
			}
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable to set numeric property type %s for property %s"), *Property->GetClass()->GetName(), *Property->GetNameCPP());
			return false;
		}
	}
	else if (UBoolProperty *BoolProperty = Cast<UBoolProperty>(Property))
	{
		// AsBool will log an error for completely inappropriate types (then give us a default)
		BoolProperty->SetPropertyValue(OutValue, JsonValue->AsBool());
	}
	else if (UStrProperty *StringProperty = Cast<UStrProperty>(Property))
	{
		// AsString will log an error for completely inappropriate types (then give us a default)
		StringProperty->SetPropertyValue(OutValue, JsonValue->AsString());
	}
	else if (UArrayProperty *ArrayProperty = Cast<UArrayProperty>(Property))
	{
		if (JsonValue->Type == EJson::Array)
		{
			TArray< TSharedPtr<FJsonValue> > ArrayValue = JsonValue->AsArray();
			int32 ArrLen = ArrayValue.Num();

			// make the output array size match
			FScriptArrayHelper Helper(ArrayProperty, OutValue);
			Helper.Resize(ArrLen);

			// set the property values
			for (int32 i = 0; i<ArrLen; ++i)
			{
				const TSharedPtr<FJsonValue>& ArrayValueItem = ArrayValue[i];
				if (ArrayValueItem.IsValid() && !ArrayValueItem->IsNull())
				{
					if (!JsonValueToUProperty(ArrayValueItem, ArrayProperty->Inner, Helper.GetRawPtr(i), CheckFlags & (~CPF_ParmFlags), SkipFlags))
					{
						//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable to deserialize array element [%d] for property %s"), i, *Property->GetNameCPP());
						return false;
					}
				}
			}
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import TArray from non-array JSON key for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else if (UMapProperty* MapProperty = Cast<UMapProperty>(Property))
	{
		if (JsonValue->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> ObjectValue = JsonValue->AsObject();

			FScriptMapHelper Helper(MapProperty, OutValue);

			// set the property values
			for (const auto& Entry : ObjectValue->Values)
			{
				if (Entry.Value.IsValid() && !Entry.Value->IsNull())
				{
					int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();

					TSharedPtr<FJsonValueString> TempKeyValue = MakeShareable(new FJsonValueString(Entry.Key));

					const bool bKeySuccess = JsonValueToUProperty(TempKeyValue, MapProperty->KeyProp, Helper.GetKeyPtr(NewIndex), CheckFlags & (~CPF_ParmFlags), SkipFlags);
					const bool bValueSuccess = JsonValueToUProperty(Entry.Value, MapProperty->ValueProp, Helper.GetValuePtr(NewIndex), CheckFlags & (~CPF_ParmFlags), SkipFlags);

					if (!(bKeySuccess && bValueSuccess))
					{
						//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable to deserialize map element [key: %s] for property %s"), *Entry.Key, *Property->GetNameCPP());
						return false;
					}
				}
			}

			Helper.Rehash();
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import TMap from non-object JSON key for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else if (USetProperty* SetProperty = Cast<USetProperty>(Property))
	{
		if (JsonValue->Type == EJson::Array)
		{
			TArray< TSharedPtr<FJsonValue> > ArrayValue = JsonValue->AsArray();
			int32 ArrLen = ArrayValue.Num();

			FScriptSetHelper Helper(SetProperty, OutValue);

			// set the property values
			for (int32 i = 0; i < ArrLen; ++i)
			{
				const TSharedPtr<FJsonValue>& ArrayValueItem = ArrayValue[i];
				if (ArrayValueItem.IsValid() && !ArrayValueItem->IsNull())
				{
					int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
					if (!JsonValueToUProperty(ArrayValueItem, SetProperty->ElementProp, Helper.GetElementPtr(NewIndex), CheckFlags & (~CPF_ParmFlags), SkipFlags))
					{
						//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable to deserialize set element [%d] for property %s"), i, *Property->GetNameCPP());
						return false;
					}
				}
			}

			Helper.Rehash();
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import TSet from non-array JSON key for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else if (UTextProperty *TextProperty = Cast<UTextProperty>(Property))
	{
		if (JsonValue->Type == EJson::String)
		{
			// assume this string is already localized, so import as invariant
			TextProperty->SetPropertyValue(OutValue, FText::FromString(JsonValue->AsString()));
		}
		else if (JsonValue->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
			check(Obj.IsValid()); // should not fail if Type == EJson::Object

								  // import the subvalue as a culture invariant string
			FText Text;
			if (!GetTextFromObject(Obj.ToSharedRef(), Text))
			{
				//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import FText from JSON object with invalid keys for property %s"), *Property->GetNameCPP());
				return false;
			}
			TextProperty->SetPropertyValue(OutValue, Text);
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import FText from JSON that was neither string nor object for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else if (UStructProperty *StructProperty = Cast<UStructProperty>(Property))
	{
		static const FName NAME_DateTime(TEXT("DateTime"));
		static const FName NAME_Color(TEXT("Color"));
		static const FName NAME_LinearColor(TEXT("LinearColor"));
		if (JsonValue->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
			check(Obj.IsValid()); // should not fail if Type == EJson::Object
			if (!JsonObjectToUStruct(Obj.ToSharedRef(), StructProperty->Struct, OutValue, CheckFlags & (~CPF_ParmFlags), SkipFlags))
			{
				//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - JsonObjectToUStruct failed for property %s"), *Property->GetNameCPP());
				return false;
			}
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_LinearColor)
		{
			FLinearColor& ColorOut = *(FLinearColor*)OutValue;
			FString ColorString = JsonValue->AsString();

			FColor IntermediateColor;
			IntermediateColor = FColor::FromHex(ColorString);

			ColorOut = IntermediateColor;
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_Color)
		{
			FColor& ColorOut = *(FColor*)OutValue;
			FString ColorString = JsonValue->AsString();

			ColorOut = FColor::FromHex(ColorString);
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetFName() == NAME_DateTime)
		{
			FString DateString = JsonValue->AsString();
			FDateTime& DateTimeOut = *(FDateTime*)OutValue;
			if (DateString == TEXT("min"))
			{
				// min representable value for our date struct. Actual date may vary by platform (this is used for sorting)
				DateTimeOut = FDateTime::MinValue();
			}
			else if (DateString == TEXT("max"))
			{
				// max representable value for our date struct. Actual date may vary by platform (this is used for sorting)
				DateTimeOut = FDateTime::MaxValue();
			}
			else if (DateString == TEXT("now"))
			{
				// this value's not really meaningful from json serialization (since we don't know timezone) but handle it anyway since we're handling the other keywords
				DateTimeOut = FDateTime::UtcNow();
			}
			else if (FDateTime::ParseIso8601(*DateString, DateTimeOut))
			{
				// ok
			}
			else if (FDateTime::Parse(DateString, DateTimeOut))
			{
				// ok
			}
			else
			{
				//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable to import FDateTime for property %s"), *Property->GetNameCPP());
				return false;
			}
		}
		else if (JsonValue->Type == EJson::String && StructProperty->Struct->GetCppStructOps() && StructProperty->Struct->GetCppStructOps()->HasImportTextItem())
		{
			UScriptStruct::ICppStructOps* TheCppStructOps = StructProperty->Struct->GetCppStructOps();

			FString ImportTextString = JsonValue->AsString();
			const TCHAR* ImportTextPtr = *ImportTextString;
			TheCppStructOps->ImportTextItem(ImportTextPtr, OutValue, PPF_None, nullptr, (FOutputDevice*)GWarn);
		}
		else
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Attempted to import UStruct from non-object JSON key for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else if (UObjectProperty *ObjectProperty = Cast<UObjectProperty>(Property) )
	{
		if (Cast<UObject>(ObjectProperty->GetObjectPropertyValue(OutValue)) == nullptr)
		{
			UObject* pSubObj = NewObject<UObject>((UObject*)GetTransientPackage(), ObjectProperty->PropertyClass);
			ObjectProperty->SetObjectPropertyValue(OutValue, pSubObj);
		}

		TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
		UObject* pNewObj = ObjectProperty->GetObjectPropertyValue(OutValue);
		check(Obj.IsValid()); // should not fail if Type == EJson::Object
		if (!JsonObjectToUStruct(Obj.ToSharedRef(), ObjectProperty->PropertyClass, pNewObj, CheckFlags & (~CPF_ParmFlags), SkipFlags))
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - JsonObjectToUStruct failed for property %s"), *Property->GetNameCPP());
			return false;
		}
	}
	else
	{
		// Default to expect a string for everything else
		if (Property->ImportText(*JsonValue->AsString(), OutValue, 0, NULL) == NULL)
		{
			//UE_LOG(LogJson, Error, TEXT("JsonValueToUProperty - Unable import property type %s from string value for property %s"), *Property->GetClass()->GetName(), *Property->GetNameCPP());
			return false;
		}
	}

	return true;
}


bool UWebSocketBlueprintLibrary::JsonObjectToUStruct(const TSharedRef<FJsonObject>& JsonObject, const UStruct* StructDefinition, void* OutStruct, int64 CheckFlags, int64 SkipFlags)
{
	return JsonAttributesToUStruct(JsonObject->Values, StructDefinition, OutStruct, CheckFlags, SkipFlags);
}

bool UWebSocketBlueprintLibrary::JsonAttributesToUStruct(const TMap< FString, TSharedPtr<FJsonValue> >& JsonAttributes, const UStruct* StructDefinition, void* OutStruct, int64 CheckFlags, int64 SkipFlags)
{
	if (StructDefinition == FJsonObjectWrapper::StaticStruct())
	{
		// Just copy it into the object
		FJsonObjectWrapper* ProxyObject = (FJsonObjectWrapper *)OutStruct;
		ProxyObject->JsonObject = MakeShareable(new FJsonObject());
		ProxyObject->JsonObject->Values = JsonAttributes;
		return true;
	}

	// iterate over the struct properties
	for (TFieldIterator<UProperty> PropIt(StructDefinition); PropIt; ++PropIt)
	{
		UProperty* Property = *PropIt;
		FString PropertyName = Property->GetName();

		// Check to see if we should ignore this property
		if (CheckFlags != 0 && !Property->HasAnyPropertyFlags(CheckFlags))
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(SkipFlags))
		{
			continue;
		}

		// find a json value matching this property name
		TSharedPtr<FJsonValue> JsonValue;
		for (auto It = JsonAttributes.CreateConstIterator(); It; ++It)
		{
			// use case insensitive search sincd FName may change caseing strangely on us
			if (PropertyName.Equals(It.Key(), ESearchCase::IgnoreCase))
			{
				JsonValue = It.Value();
				break;
			}
		}
		if (!JsonValue.IsValid() || JsonValue->IsNull())
		{
			// we allow values to not be found since this mirrors the typical UObject mantra that all the fields are optional when deserializing
			continue;
		}

		void* Value = Property->ContainerPtrToValuePtr<uint8>(OutStruct);
		if (!JsonValueToUProperty(JsonValue, Property, Value, CheckFlags, SkipFlags))
		{
			UE_LOG(LogJson, Error, TEXT("JsonObjectToUStruct - Unable to parse %s.%s from JSON"), *StructDefinition->GetName(), *PropertyName);
			return false;
		}
	}

	return true;
}


bool UWebSocketBlueprintLibrary::UObjectToJsonObject(const UStruct* StructDefinition, const void* Struct, TSharedRef<FJsonObject> OutJsonObject, int64 CheckFlags, int64 SkipFlags)
{
	return UObjectToJsonAttributes(StructDefinition, Struct, OutJsonObject->Values, CheckFlags, SkipFlags);
}

bool UWebSocketBlueprintLibrary::UObjectToJsonAttributes(const UStruct* StructDefinition, const void* Struct, TMap< FString, TSharedPtr<FJsonValue> >& OutJsonAttributes, int64 CheckFlags, int64 SkipFlags)
{
	if (SkipFlags == 0)
	{
		// If we have no specified skip flags, skip deprecated, transient and skip serialization by default when writing
		SkipFlags |= CPF_Deprecated | CPF_Transient;
	}

	if (StructDefinition == FJsonObjectWrapper::StaticStruct())
	{
		// Just copy it into the object
		const FJsonObjectWrapper* ProxyObject = (const FJsonObjectWrapper *)Struct;

		if (ProxyObject->JsonObject.IsValid())
		{
			OutJsonAttributes = ProxyObject->JsonObject->Values;
		}
		return true;
	}

	for (TFieldIterator<UProperty> It(StructDefinition); It; ++It)
	{
		UProperty* Property = *It;

		// Check to see if we should ignore this property
		if (CheckFlags != 0 && !Property->HasAnyPropertyFlags(CheckFlags))
		{
			continue;
		}
		if (Property->HasAnyPropertyFlags(SkipFlags))
		{
			continue;
		}

		FString VariableName = Property->GetName();
		const void* Value = Property->ContainerPtrToValuePtr<uint8>(Struct);

		// convert the property to a FJsonValue
		TSharedPtr<FJsonValue> JsonValue = UPropertyToJsonValue(Property, Value, CheckFlags, SkipFlags);
		if (!JsonValue.IsValid())
		{
			//UClass* PropClass = Property->GetClass();
			FFieldClass* PropClass = Property->GetClass();
			UE_LOG(WebSocket, Warning, TEXT("UObjectToJsonObject - Unhandled property type '%s': %s"), *PropClass->GetName(), *Property->GetPathName());
			continue;
		}

		// set the value on the output object
		OutJsonAttributes.Add(VariableName, JsonValue);
	}

	return true;
}

TSharedPtr<FJsonValue> UWebSocketBlueprintLibrary::UPropertyToJsonValue(UProperty* Property, const void* Value, int64 CheckFlags, int64 SkipFlags)
{
	if (Property->ArrayDim == 1)
	{
		return ConvertScalarUPropertyToJsonValue(Property, Value, CheckFlags, SkipFlags);
	}

	TArray< TSharedPtr<FJsonValue> > Array;
	for (int Index = 0; Index != Property->ArrayDim; ++Index)
	{
		Array.Add(ConvertScalarUPropertyToJsonValue(Property, (char*)Value + Index * Property->ElementSize, CheckFlags, SkipFlags));
	}
	return MakeShareable(new FJsonValueArray(Array));
}

TSharedPtr<FJsonValue> UWebSocketBlueprintLibrary::ConvertScalarUPropertyToJsonValue(UProperty* Property, const void* Value, int64 CheckFlags, int64 SkipFlags)
{
	// See if there's a custom export callback first, so it can override default behavior
	

	if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
	{
		// export enums as strings
		UEnum* EnumDef = EnumProperty->GetEnum();
#if ENGINE_MINOR_VERSION > 15
		FString StringValue = EnumDef->GetNameStringByValue(EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
        
#else
    FString StringValue = EnumDef->GetEnumName(EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
#endif
		return MakeShareable(new FJsonValueString(StringValue));
	}
	else if (UNumericProperty *NumericProperty = Cast<UNumericProperty>(Property))
	{
		// see if it's an enum
		UEnum* EnumDef = NumericProperty->GetIntPropertyEnum();
		if (EnumDef != NULL)
		{
			// export enums as strings
#if ENGINE_MINOR_VERSION > 15
			FString StringValue = EnumDef->GetNameStringByValue(NumericProperty->GetSignedIntPropertyValue(Value));
#else
            FString StringValue = EnumDef->GetEnumName(EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
#endif
			return MakeShareable(new FJsonValueString(StringValue));
		}

		// We want to export numbers as numbers
		if (NumericProperty->IsFloatingPoint())
		{
			return MakeShareable(new FJsonValueNumber(NumericProperty->GetFloatingPointPropertyValue(Value)));
		}
		else if (NumericProperty->IsInteger())
		{
			return MakeShareable(new FJsonValueNumber(NumericProperty->GetSignedIntPropertyValue(Value)));
		}

		// fall through to default
	}
	else if (UBoolProperty *BoolProperty = Cast<UBoolProperty>(Property))
	{
		// Export bools as bools
		return MakeShareable(new FJsonValueBoolean(BoolProperty->GetPropertyValue(Value)));
	}
	else if (UStrProperty *StringProperty = Cast<UStrProperty>(Property))
	{
		return MakeShareable(new FJsonValueString(StringProperty->GetPropertyValue(Value)));
	}
	else if (UTextProperty *TextProperty = Cast<UTextProperty>(Property))
	{
		return MakeShareable(new FJsonValueString(TextProperty->GetPropertyValue(Value).ToString()));
	}
	else if (UArrayProperty *ArrayProperty = Cast<UArrayProperty>(Property))
	{
		TArray< TSharedPtr<FJsonValue> > Out;
		FScriptArrayHelper Helper(ArrayProperty, Value);
		for (int32 i = 0, n = Helper.Num(); i<n; ++i)
		{
			TSharedPtr<FJsonValue> Elem = UPropertyToJsonValue(ArrayProperty->Inner, Helper.GetRawPtr(i), CheckFlags & (~CPF_ParmFlags), SkipFlags);
			if (Elem.IsValid())
			{
				// add to the array
				Out.Push(Elem);
			}
		}
		return MakeShareable(new FJsonValueArray(Out));
	}
	else if (USetProperty* SetProperty = Cast<USetProperty>(Property))
	{
		TArray< TSharedPtr<FJsonValue> > Out;
		FScriptSetHelper Helper(SetProperty, Value);
		for (int32 i = 0, n = Helper.Num(); i < n; ++i)
		{
			if (Helper.IsValidIndex(i))
			{
				TSharedPtr<FJsonValue> Elem = UPropertyToJsonValue(SetProperty->ElementProp, Helper.GetElementPtr(i), CheckFlags & (~CPF_ParmFlags), SkipFlags);
				if (Elem.IsValid())
				{
					// add to the array
					Out.Push(Elem);
				}
			}
		}
		return MakeShareable(new FJsonValueArray(Out));
	}
	else if (UMapProperty* MapProperty = Cast<UMapProperty>(Property))
	{
		TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());

		FScriptMapHelper Helper(MapProperty, Value);
		for (int32 i = 0, n = Helper.Num(); i < n; ++i)
		{
			if (Helper.IsValidIndex(i))
			{
				TSharedPtr<FJsonValue> KeyElement = UPropertyToJsonValue(MapProperty->KeyProp, Helper.GetKeyPtr(i), CheckFlags & (~CPF_ParmFlags), SkipFlags);
				TSharedPtr<FJsonValue> ValueElement = UPropertyToJsonValue(MapProperty->ValueProp, Helper.GetValuePtr(i), CheckFlags & (~CPF_ParmFlags), SkipFlags);
				if (KeyElement.IsValid() && ValueElement.IsValid())
				{
					Out->SetField(KeyElement->AsString(), ValueElement);
				}
			}
		}

		return MakeShareable(new FJsonValueObject(Out));
	}
	else if (UStructProperty *StructProperty = Cast<UStructProperty>(Property))
	{
		UScriptStruct::ICppStructOps* TheCppStructOps = StructProperty->Struct->GetCppStructOps();
		// Intentionally exclude the JSON Object wrapper, which specifically needs to export JSON in an object representation instead of a string
		if (StructProperty->Struct != FJsonObjectWrapper::StaticStruct() && TheCppStructOps && TheCppStructOps->HasExportTextItem())
		{
			FString OutValueStr;
			TheCppStructOps->ExportTextItem(OutValueStr, Value, nullptr, nullptr, PPF_None, nullptr);
			return MakeShareable(new FJsonValueString(OutValueStr));
		}

		TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());
		if (UObjectToJsonObject(StructProperty->Struct, Value, Out, CheckFlags & (~CPF_ParmFlags), SkipFlags))
		{
			return MakeShareable(new FJsonValueObject(Out));
		}
		// fall through to default
	}
	else if (UObjectProperty* ObjectProperty = Cast<UObjectProperty>(Property))
	{
		UObject* tmpValue = Cast<UObject>(ObjectProperty->GetObjectPropertyValue(Value));
		
		if (tmpValue != nullptr)
		{
			TSharedRef<FJsonObject> Out = MakeShareable(new FJsonObject());
			if (UObjectToJsonObject(ObjectProperty->PropertyClass, tmpValue, Out, CheckFlags & (~CPF_ParmFlags), SkipFlags))
			{
				return MakeShareable(new FJsonValueObject(Out));
			}
		}
	}
	else
	{
		// Default to export as string for everything else
		FString StringValue;
		Property->ExportTextItem(StringValue, Value, NULL, NULL, PPF_None);
		return MakeShareable(new FJsonValueString(StringValue));
	}

	// invalid
	return TSharedPtr<FJsonValue>();
}


