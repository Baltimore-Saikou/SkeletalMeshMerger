// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AnimToTextureDataAsset.h"
#include "SkeletalMergingLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UObject/SavePackage.h"
#include "JrSkeletalMergingLibrary.generated.h"

class USCS_Node;

/**
* Component that can be used to perform Skeletal Mesh merges from Blueprints.
*/
UCLASS( ClassGroup=(Custom) )
class JRSKELETALMESHMERGER_API UJrSkeletalMergingLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "SkelMerge", meta = (UnsafeDuringActorConstruction = "true"))
	static bool SaveMergeSkeletons(const FSkeletonMergeParams& mergeParams, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath, USkeleton* &ResultMesh);

	UFUNCTION(BlueprintCallable, Category = "SkelMerge", meta = (UnsafeDuringActorConstruction = "true"))
	static bool SaveMergeMeshes(const FSkeletalMeshMergeParams& mergeParams, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath, USkeletalMesh* &ResultMesh);

	static void CreateComponentsByNode(USCS_Node* RootNode, UBlueprint* NewBlueprint);

	/**
	 * @param SkelMesh 合并后的Mesh
	 * @param ChildSkelMesh 不需要合并的Mesh
	 * @param ActorClass 原蓝图类
	 * @param fileName 新创建的资产名
	 * @param AbsolutePath 资产路径
	 */
	UFUNCTION(BlueprintCallable, Category = "SkelMerge", meta = (UnsafeDuringActorConstruction = "true"))
	static bool CreateBlueprintAssetAfterMerging(USkeletalMesh* SkelMesh, TArray<USkeletalMesh*> ChildSkelMesh, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath);

	static TArray<USCS_Node*> GetSkeletalNodesByClass(const TSubclassOf<AActor> ActorClass);

	template<class T>
	static TArray<USCS_Node*> GetNodesByClass(const TSubclassOf<AActor> ActorClass)
	{
		TArray<USCS_Node*> Nodes;

		if (const TSubclassOf<AActor> ParentClass = Cast<UBlueprintGeneratedClass>(ActorClass->GetSuperStruct()))
		{
			Nodes.Append(GetNodesByClass<T>(ParentClass));
		}
	
		if (const UBlueprintGeneratedClass* ActorBlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ActorClass))
		{
			const TArray<USCS_Node*>& ActorBlueprintNodes = ActorBlueprintGeneratedClass->SimpleConstructionScript->GetAllNodes();
			for(USCS_Node* Node : ActorBlueprintNodes)
			{
				if(Node->ComponentClass->IsChildOf(T::StaticClass()))
				{
					Nodes.Add(Node);
				}
			}
		}
	
		return Nodes;
	}

	template<class T>
	static TArray<T*> GetComponentsByClass(const TSubclassOf<AActor> ActorClass)
	{
		TArray<T*> ActorComponents;
		if(const UBlueprintGeneratedClass* ActorBlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ActorClass))
		{
			ActorBlueprintGeneratedClass->SimpleConstructionScript->GetComponentEditorActorInstance()->GetComponents<T>(ActorComponents);
		}
		return ActorComponents;
	}

	UFUNCTION(BlueprintCallable, Category = "SkelMerge", meta = (UnsafeDuringActorConstruction = "true"))
	static void BoneNameCheck(TArray<USkeleton*> Skeletons);

    static void MergeSkeletal(FSkeletalMeshMergeParams& SkeletalMeshMergeParams, const FSkeletonMergeParams& SkeletonMergeParams, TArray<USCS_Node*> SkeletalNodes);

	static USkeleton* MergeSkeletons(const FSkeletonMergeParams& Params, TArray<USCS_Node*> SkeletalNodes);

	static USkeletalMesh* MergeMeshes(const FSkeletalMeshMergeParams& Params);

	UFUNCTION(BlueprintCallable, Category="Mesh Merge", meta=(UnsafeDuringActorConstruction="true"))
	static TArray<USkeletalMeshComponent*> GetSkeletalMeshByClass(const TSubclassOf<AActor> ActorClass);

	UFUNCTION(BlueprintCallable)
	static UObject* CreateAsset(UObject* Obj, const FString& fileName, const FString& AbsolutePath);
	
	UFUNCTION(BlueprintCallable)
	static void SaveAssetsOfClass(UClass* AssetClass);

	UFUNCTION(BlueprintCallable, BlueprintPure)
	static FMeshBuildSettings GetBuildSettingsFromStaticMesh(UStaticMesh* StaticMesh, const int32 LODIndex);

	UFUNCTION(BlueprintCallable)
	static UMaterialInstanceConstant* CreateMIC_EditorOnly(UMaterialInterface* Material, FString Name = "MIC_");


	
	/* 更新材质实例 */

	/**
	* Updates a material's parameters to match those of an animToTexture data asset
	*/
	UFUNCTION(BlueprintCallable, meta = (Category = "AnimToTexture"))
	static void UpdateMaterialInstanceFromDataAsset(UAnimToTextureDataAsset* DataAsset, class UMaterialInstanceConstant* MaterialInstance, 
		const bool bAnimate=false, const EAnimToTextureNumBoneInfluences NumBoneInfluences = EAnimToTextureNumBoneInfluences::One,
		const EMaterialParameterAssociation MaterialParameterAssociation = EMaterialParameterAssociation::LayerParameter);

	/** Get the current scalar (float) parameter value from a Material Instance */
	static float GetMaterialInstanceScalarParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Set the scalar (float) parameter value for a Material Instance */
	static bool SetMaterialInstanceScalarParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, float Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Get the current texture parameter value from a Material Instance */
	static UTexture* GetMaterialInstanceTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Set the texture parameter value for a Material Instance */
	static bool SetMaterialInstanceTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, UTexture* Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Get the current texture parameter value from a Material Instance */
	static URuntimeVirtualTexture* GetMaterialInstanceRuntimeVirtualTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	UFUNCTION(BlueprintCallable, Category = "MaterialEditing")
	static bool SetMaterialInstanceRuntimeVirtualTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, URuntimeVirtualTexture* Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Get the current texture parameter value from a Material Instance */
	static USparseVolumeTexture* GetMaterialInstanceSparseVolumeTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Set the texture parameter value for a Material Instance */
	static bool SetMaterialInstanceSparseVolumeTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, USparseVolumeTexture* Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Get the current vector parameter value from a Material Instance */
	static FLinearColor GetMaterialInstanceVectorParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Set the vector parameter value for a Material Instance */
	static bool SetMaterialInstanceVectorParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, FLinearColor Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Get the current static switch parameter value from a Material Instance */
	static bool GetMaterialInstanceStaticSwitchParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	/** Set the static switch parameter value for a Material Instance */
	static bool SetMaterialInstanceStaticSwitchParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, bool Value, EMaterialParameterAssociation Association = GlobalParameter, int32 LayerIndex = 0);

	UFUNCTION(BlueprintCallable, Category = "MaterialEditing")
	static TArray<UMaterialFunctionInterface*> GetMaterialLayers(UMaterialInstanceConstant* Instance);

	UFUNCTION(BlueprintCallable, Category = "MaterialEditing")
	static void AddMaterialLayer(UMaterialInstanceConstant* Instance, UMaterialFunctionInterface* MaterialLayer, UMaterialFunctionInterface* BlendLayer, bool bVisible = true, EMaterialLayerLinkState LinkState = EMaterialLayerLinkState::NotFromParent);

	UFUNCTION(BlueprintCallable, Category = "MaterialEditing")
	static bool UpdateMaterialLayers(UMaterialInstanceConstant* Instance, UMaterialFunctionInterface* MaterialLayer, UMaterialFunctionInterface* BlendLayer, bool bVisible = true, EMaterialLayerLinkState LinkState = EMaterialLayerLinkState::NotFromParent);

	/** 
	* Utility for converting SkeletalMesh into a StaticMesh
	*/
	UFUNCTION(BlueprintCallable, Category = "MaterialEditing")
	static UStaticMesh* ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkeletalMesh, const FString PackageName, const int32 LODIndex = -1);

	

protected:
	static void AddSockets(USkeleton* InSkeleton, const TArray<TObjectPtr<USkeletalMeshSocket>>& InSockets);
	static void AddVirtualBones(USkeleton* InSkeleton, const TArray<const FVirtualBone*> InVirtualBones);
	static void AddCurveNames(USkeleton* InSkeleton, const TMap<FName, const FCurveMetaData*>& InCurves);
	static void AddBlendProfiles(USkeleton* InSkeleton, const TMap<FName, TArray<const UBlendProfile*>>& InBlendProfiles);
	static void AddAnimationSlotGroups(USkeleton* InSkeleton, const TMap<FName, TSet<FName>>& InSlotGroupsNames);
};

