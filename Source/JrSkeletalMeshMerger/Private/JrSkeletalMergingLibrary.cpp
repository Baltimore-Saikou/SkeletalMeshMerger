// Copyright Epic Games, Inc. All Rights Reserved.

#include "JrSkeletalMergingLibrary.h"
#include "MaterialEditingLibrary.h"
#include "AssetToolsModule.h"
#include "BoneWeights.h"
#include "ClothingAsset.h"
#include "EditorAssetLibrary.h"
#include "EditorDialogLibrary.h"
#include "IAssetTools.h"
#include "JrSkeletalMeshMergeFunc.h"
#include "SkeletalMeshAttributes.h"
#include "SkinnedAssetCompiler.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkeletalMesh.h"
#include "Algo/Accumulate.h"
#include "Animation/BlendProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/SavePackage.h"
#include "MeshDescription.h"
#include "PackageTools.h"
#include "Engine/AssetManager.h"
#include "Engine/InheritableComponentHandler.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialEditor/MaterialEditorInstanceConstant.h"
#include "Materials/MaterialInstanceConstant.h"

#define LOCTEXT_NAMESPACE "JrSkeletalMergingLibrary"
#include "MeshUtilities.h"
#include "RawMesh.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(JrSkeletalMergingLibrary)
UE_DISABLE_OPTIMIZATION
DEFINE_LOG_CATEGORY(LogSkeletalMeshMerge);

namespace UE
{
	namespace SkeletonMerging
	{
		// Helper structure to merge bone hierarchies together and populate a FReferenceSkeleton with the result(s)
		struct FMergedBoneHierarchy
		{
			FMergedBoneHierarchy(uint32 NumExpectedBones)
			{
				BoneNamePose.Reserve(NumExpectedBones);
				PathToBoneNames.Reserve(NumExpectedBones);
				PathHashToBoneNames.Reserve(NumExpectedBones);
			}
			
			void AddBone(const FName& BoneName, const FTransform& ReferencePose, uint32 PathHash)
			{
				// Store reference transform according to bone name hash
				BoneNamePose.Add(BoneName, ReferencePose);

				// Add bone as child to parent path
				PathHashToBoneNames.FindOrAdd(PathHash).Add(BoneName);

				// Append bone hash to parent path and store 
				const uint32 BoneHash = GetTypeHash(BoneName);
				PathToBoneNames.Add(BoneName, HashCombine(PathHash, BoneHash));
			}

			void PopulateSkeleton(FReferenceSkeletonModifier& SkeletonModifier)
			{
				const uint32 Zero = 0;
				const uint32 RootParentHash = HashCombine(Zero, Zero);

				// Root bone is always parented to 0 hash data entry, so we expect a single root-bone (child)
				const TSet<FName>& ChildBoneNames = GetChildBonesForPath(RootParentHash);

				// ensure(ChildBoneNames.Num() == 1);
				const FName RootBoneName = ChildBoneNames.Array()[0];

				// Add root-bone and traverse data to populate child hierarchies
				const FMeshBoneInfo BoneInfo(RootBoneName, RootBoneName.ToString(), INDEX_NONE);
				SkeletonModifier.Add(BoneInfo, GetReferencePose(RootBoneName));

				RecursiveAddBones(SkeletonModifier, RootBoneName);
			}
		protected:			
			const FTransform& GetReferencePose(const FName& InName) const { return BoneNamePose.FindChecked(InName); }
			uint32 GetBonePathHash(const FName& InName) const { return PathToBoneNames.FindChecked(InName); }
			const TSet<FName>* FindChildBonesForPath(uint32 InPath) const { return PathHashToBoneNames.Find(InPath); }
			const TSet<FName>& GetChildBonesForPath(uint32 InPath) const { return PathHashToBoneNames.FindChecked(InPath); }
			
			void RecursiveAddBones(FReferenceSkeletonModifier& SkeletonModifier, const FName ParentBoneName)
			{
				const uint32 PathHash = GetBonePathHash(ParentBoneName);
				if (const TSet<FName>* BoneNamesPtr = FindChildBonesForPath(PathHash))
				{
					for (const FName& ChildBoneName : *BoneNamesPtr)
					{
						FMeshBoneInfo BoneInfo(ChildBoneName, ChildBoneName.ToString(), SkeletonModifier.FindBoneIndex(ParentBoneName));
						SkeletonModifier.Add(BoneInfo, GetReferencePose(ChildBoneName));
						RecursiveAddBones(SkeletonModifier, ChildBoneName);
					}
				}
			}

		private:			
			// Reference pose transform for given bone name
			TMap<FName, FTransform> BoneNamePose;
			// Accumulated hierarchy hash from bone to root bone
			TMap<FName, uint32> PathToBoneNames;
			// Set of child bones for given hierarchy hash
			TMap<uint32, TSet<FName>> PathHashToBoneNames;
		};
	}
}


void GetMeshDescription(FMeshDescription& MeshDescription, const USkeletalMesh *Owner, FSkeletalMeshLODModel* LODModel)
{
	using UE::AnimationCore::FBoneWeights;

	MeshDescription.Empty();
	
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);			
	
	// Register extra attributes for us.
	MeshAttributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = MeshAttributes.GetVertexPositions();
	FSkinWeightsVertexAttributesRef VertexSkinWeights = MeshAttributes.GetVertexSkinWeights();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceNormals = MeshAttributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexInstanceTangents = MeshAttributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexInstanceBinormalSigns = MeshAttributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> VertexInstanceColors = MeshAttributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> VertexInstanceUVs = MeshAttributes.GetVertexInstanceUVs();

	TPolygonGroupAttributesRef<FName> PolygonGroupMaterialSlotNames = MeshAttributes.GetPolygonGroupMaterialSlotNames();
	
	const int32 NumTriangles = LODModel->IndexBuffer.Num() / 3;

	MeshDescription.ReserveNewPolygonGroups(LODModel->Sections.Num());
	MeshDescription.ReserveNewTriangles(NumTriangles);
	MeshDescription.ReserveNewVertexInstances(NumTriangles * 3);
	MeshDescription.ReserveNewVertices(static_cast<int32>(LODModel->NumVertices));

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(LODModel->NumVertices);
	for (int32 VertexIndex = 0; VertexIndex < int32(LODModel->NumVertices); VertexIndex++)
	{
		VertexIDs.Add(MeshDescription.CreateVertex());
	}

	VertexInstanceUVs.SetNumChannels(LODModel->NumTexCoords);
	
	const TArray<FSkeletalMaterial>& Materials = Owner->GetMaterials();
	const bool bHasVertexColors = (bool)(Owner->GetVertexBufferFlags() & ESkeletalMeshVertexFlags::HasVertexColors);

	// Convert sections to polygon groups, each with their own material.
	for (int32 SectionIndex = 0; SectionIndex < LODModel->Sections.Num(); SectionIndex++)
	{
		const FSkelMeshSection& Section = LODModel->Sections[SectionIndex];

		// Convert positions and bone weights
		const TArray<FSoftSkinVertex>& SourceVertices = Section.SoftVertices;
		for (int32 VertexIndex = 0; VertexIndex < SourceVertices.Num(); VertexIndex++)
		{
			const FVertexID VertexID = VertexIDs[VertexIndex + Section.BaseVertexIndex];

			VertexPositions.Set(VertexID, SourceVertices[VertexIndex].Position);

			// Skeleton bone indexes translated from the render mesh compact indexes.
			FBoneIndexType	InfluenceBones[MAX_TOTAL_INFLUENCES];

			for (int32 InfluenceIndex = 0; InfluenceIndex < MAX_TOTAL_INFLUENCES && SourceVertices[VertexIndex].InfluenceWeights[InfluenceIndex]; InfluenceIndex++)
			{
				const int32 BoneId = SourceVertices[VertexIndex].InfluenceBones[InfluenceIndex];

				InfluenceBones[InfluenceIndex] = Section.BoneMap[BoneId];
			}

			VertexSkinWeights.Set(VertexID, FBoneWeights::Create(InfluenceBones, SourceVertices[VertexIndex].InfluenceWeights));
		}


		const FPolygonGroupID PolygonGroupID(Section.MaterialIndex);

		if (!MeshDescription.IsPolygonGroupValid(PolygonGroupID))
		{
			MeshDescription.CreatePolygonGroupWithID(PolygonGroupID);
		}

		if (ensure(Materials.IsValidIndex(Section.MaterialIndex)))
		{
			PolygonGroupMaterialSlotNames.Set(PolygonGroupID, Materials[Section.MaterialIndex].ImportedMaterialSlotName);
		}

		for (int32 TriangleID = 0; TriangleID < int32(Section.NumTriangles); TriangleID++)
		{
			const int32 VertexIndexBase = TriangleID * 3 + Section.BaseIndex;

			TArray<FVertexInstanceID> TriangleVertexInstanceIDs;
			TriangleVertexInstanceIDs.SetNum(3);

			for (int32 Corner = 0; Corner < 3; Corner++)
			{
				const int32 SourceVertexIndex = LODModel->IndexBuffer[VertexIndexBase + Corner];
				const FVertexID VertexID = VertexIDs[SourceVertexIndex];
				const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);

				const FSoftSkinVertex& SourceVertex = SourceVertices[SourceVertexIndex - Section.BaseVertexIndex];

				VertexInstanceNormals.Set(VertexInstanceID, SourceVertex.TangentZ);
				VertexInstanceTangents.Set(VertexInstanceID, SourceVertex.TangentX);
				VertexInstanceBinormalSigns.Set(VertexInstanceID, FMatrix44f(
					SourceVertex.TangentX.GetSafeNormal(),
					SourceVertex.TangentY.GetSafeNormal(),
					(FVector3f)(SourceVertex.TangentZ.GetSafeNormal()),
					FVector3f::ZeroVector).Determinant() < 0.0f ? -1.0f : +1.0f);

				for (int32 UVIndex = 0; UVIndex < int32(LODModel->NumTexCoords); UVIndex++)
				{
					VertexInstanceUVs.Set(VertexInstanceID, UVIndex, SourceVertex.UVs[UVIndex]);
				}

				if (bHasVertexColors)
				{
					VertexInstanceColors.Set(VertexInstanceID, FVector4f(FLinearColor(SourceVertex.Color)));
				}

				TriangleVertexInstanceIDs[Corner] = VertexInstanceID;
			}

			MeshDescription.CreateTriangle(PolygonGroupID, TriangleVertexInstanceIDs);
		}
	}
}


void GenerateImportedModel(USkeletalMesh* SkeletalMesh)
{
#if WITH_EDITORONLY_DATA
    FSkeletalMeshRenderData* SkelResource = SkeletalMesh->GetResourceForRendering();
    if (!SkelResource) {
        return;
    }

    for (UClothingAssetBase* ClothingAssetBase : SkeletalMesh->GetMeshClothingAssets()) {
        if (!ClothingAssetBase) {
            continue;
        }

        UClothingAssetCommon* ClothAsset = Cast<UClothingAssetCommon>(ClothingAssetBase);

        if (!ClothAsset) {
            continue;
        }

        if (!ClothAsset->LodData.Num())
		{
            continue;
        }

        for (FClothLODDataCommon& ClothLodData : ClothAsset->LodData)
		{
            ClothLodData.PointWeightMaps.Empty(16);
            for (TPair<uint32, FPointWeightMap>& WeightMap : ClothLodData.PhysicalMeshData.WeightMaps) {
                if (WeightMap.Value.Num()) {
                    FPointWeightMap& PointWeightMap = ClothLodData.PointWeightMaps.AddDefaulted_GetRef();
                    PointWeightMap.Initialize(WeightMap.Value, WeightMap.Key);
                }
            }
        }
    }

    FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
    ImportedModel->bGuidIsHash = false;
    ImportedModel->SkeletalMeshModelGUID = FGuid::NewGuid();

    ImportedModel->LODModels.Empty();

    int32 OriginalIndex = 0;
    for (int32 LODIndex = 0; LODIndex < SkelResource->LODRenderData.Num(); ++LODIndex) {
        ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());

        FSkeletalMeshLODRenderData& LODModel = SkelResource->LODRenderData[LODIndex];
        const int32 UVTexNum = LODModel.GetNumTexCoords();
        ImportedModel->LODModels[LODIndex].ActiveBoneIndices = LODModel.ActiveBoneIndices;
        ImportedModel->LODModels[LODIndex].NumTexCoords = LODModel.GetNumTexCoords();
        ImportedModel->LODModels[LODIndex].RequiredBones = LODModel.RequiredBones;
        ImportedModel->LODModels[LODIndex].NumVertices = LODModel.GetNumVertices();

        int indexCount = LODModel.MultiSizeIndexContainer.GetIndexBuffer()->Num();
        ImportedModel->LODModels[LODIndex].IndexBuffer.SetNum(indexCount);
        for (int i = 0; i < indexCount; ++i) {
            ImportedModel->LODModels[LODIndex].IndexBuffer[i] = LODModel.MultiSizeIndexContainer.GetIndexBuffer()->Get(i);
        }

        ImportedModel->LODModels[LODIndex].Sections.SetNum(LODModel.RenderSections.Num());

        // sections
        for (int SectionIndex = 0; SectionIndex < LODModel.RenderSections.Num(); ++SectionIndex) {
            const FSkelMeshRenderSection& RenderSection = LODModel.RenderSections[SectionIndex];
            FSkelMeshSection& ImportedSection = ImportedModel->LODModels[LODIndex].Sections[SectionIndex];

            ImportedSection.CorrespondClothAssetIndex = RenderSection.CorrespondClothAssetIndex;
            ImportedSection.ClothingData = RenderSection.ClothingData;

            if (RenderSection.ClothMappingDataLODs.Num()) {
                ImportedSection.ClothMappingDataLODs.SetNum(1);
                ImportedSection.ClothMappingDataLODs[0] = RenderSection.ClothMappingDataLODs[0];
            }

            ImportedSection.NumVertices = RenderSection.NumVertices;
            ImportedSection.NumTriangles = RenderSection.NumTriangles;
            ImportedSection.BaseIndex = RenderSection.BaseIndex;
            ImportedSection.BaseVertexIndex = RenderSection.BaseVertexIndex;
            ImportedSection.BoneMap = RenderSection.BoneMap;
            ImportedSection.MaterialIndex = RenderSection.MaterialIndex;
            ImportedSection.MaxBoneInfluences = RenderSection.MaxBoneInfluences;
            ImportedSection.SoftVertices.Empty(RenderSection.NumVertices);
            ImportedSection.SoftVertices.AddUninitialized(RenderSection.NumVertices);
            ImportedSection.bUse16BitBoneIndex = LODModel.DoesVertexBufferUse16BitBoneIndex();

            ImportedSection.OriginalDataSectionIndex = OriginalIndex++;
            FSkelMeshSourceSectionUserData& SectionUserData = ImportedModel->LODModels[LODIndex].UserSectionsData.FindOrAdd(ImportedSection.OriginalDataSectionIndex);

            SectionUserData.CorrespondClothAssetIndex = RenderSection.CorrespondClothAssetIndex;
            SectionUserData.ClothingData.AssetGuid = RenderSection.ClothingData.AssetGuid;
            SectionUserData.ClothingData.AssetLodIndex = RenderSection.ClothingData.AssetLodIndex;
        }

        // vertex data
        for (int SectionIndex = 0; SectionIndex < LODModel.RenderSections.Num(); ++SectionIndex) {
            const FSkelMeshRenderSection& RenderSection = LODModel.RenderSections[SectionIndex];
            FSkelMeshSection& ImportedSection = ImportedModel->LODModels[LODIndex].Sections[SectionIndex];
        
            for (uint32 SectionTriangleIndex = 0; SectionTriangleIndex < RenderSection.NumTriangles; ++SectionTriangleIndex) {
                for (int32 CornerIndex = 0; CornerIndex < 3; ++CornerIndex) {
                    const int32 Index = RenderSection.BaseIndex + ((SectionTriangleIndex * 3) + CornerIndex);
                    const int32 WedgeIndex = LODModel.MultiSizeIndexContainer.GetIndexBuffer()->Get(Index);
        
                    int32 SectionID = 0;
                    int32 LocalVertIndex = 0;
                    ImportedModel->LODModels[LODIndex].GetSectionFromVertexIndex(WedgeIndex, SectionID, LocalVertIndex);
                    FSoftSkinVertex& Vertex = ImportedSection.SoftVertices[LocalVertIndex];
        
                    Vertex.Position = LODModel.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(WedgeIndex);
                    Vertex.TangentX = LODModel.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(WedgeIndex);
                    Vertex.TangentY = LODModel.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentY(WedgeIndex);
                    Vertex.TangentZ = LODModel.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(WedgeIndex);
        
                    if (static_cast<uint32>(WedgeIndex) < LODModel.StaticVertexBuffers.ColorVertexBuffer.GetNumVertices()) {
                        Vertex.Color = LODModel.StaticVertexBuffers.ColorVertexBuffer.VertexColor(WedgeIndex);
                    } else {
                        Vertex.Color = FColor::White;
                    }
        
                    for (int32 UVIndex = 0; UVIndex < UVTexNum; ++UVIndex) {
                        Vertex.UVs[UVIndex] = LODModel.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(WedgeIndex, UVIndex);
                    }
        
                    for (int32 j = 0; j < RenderSection.MaxBoneInfluences; ++j) {
                        Vertex.InfluenceBones[j] = LODModel.SkinWeightVertexBuffer.GetBoneIndex(WedgeIndex, j);
                        Vertex.InfluenceWeights[j] = LODModel.SkinWeightVertexBuffer.GetBoneWeight(WedgeIndex, j);
                    }
        
                    for (int32 j = RenderSection.MaxBoneInfluences; j < MAX_TOTAL_INFLUENCES; ++j) {
                        Vertex.InfluenceBones[j] = 0;
                        Vertex.InfluenceWeights[j] = 0;
                    }
                    //Added vertex map. this is used internally everywhere in the engine. It maps LOD Model vertex data to import data.
                    ImportedModel->LODModels[LODIndex].MeshToImportVertexMap.Add(Index);
                }
            }
        }

        ImportedModel->LODModels[LODIndex].SyncronizeUserSectionsDataArray();

        const USkeletalMeshLODSettings* LODSettings = SkeletalMesh->GetLODSettings();
        const bool bValidLODSettings = LODSettings && LODSettings->GetNumberOfSettings() > LODIndex;
        const FSkeletalMeshLODGroupSettings* SkeletalMeshLODGroupSettings = bValidLODSettings ? &LODSettings->GetSettingsForLODLevel(LODIndex) : nullptr;

        FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(LODIndex);
        LODInfo->BuildGUID = LODInfo->ComputeDeriveDataCacheKey(SkeletalMeshLODGroupSettings);

        ImportedModel->LODModels[LODIndex].BuildStringID = ImportedModel->LODModels[LODIndex].GetLODModelDeriveDataKey();

        FMeshDescription MeshDescription;
        GetMeshDescription(MeshDescription, SkeletalMesh, &ImportedModel->LODModels[LODIndex]);

        FSkeletalMeshImportData MeshImportData = FSkeletalMeshImportData::CreateFromMeshDescription(MeshDescription);
        SkeletalMesh->SaveLODImportedData(LODIndex, MeshImportData);
    
    }
#endif
}

void UJrSkeletalMergingLibrary::SaveMergeSkeletal(FSkeletalMeshMergeParams& SkeletalMeshMergeParams, FSkeletonMergeParams& SkeletonMergeParams, TArray<USCS_Node*> SkeletalNodes)
{
	MergeSkeletal(SkeletalMeshMergeParams, SkeletonMergeParams, SkeletalNodes);
}

bool UJrSkeletalMergingLibrary::SaveMergeSkeletons(const FSkeletonMergeParams& mergeParams, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath, USkeleton* &ResultMesh)
{
	const FString PackagePath = AbsolutePath + fileName;

	FString FixedPackageName;

	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, FixedPackageName))
	{
		UEditorDialogLibrary::ShowMessage(FText::FromString("Skel Merging"), FText::FromString("Invalid export path!"), EAppMsgType::Ok);
		return false;
	}

	UPackage* Package = CreatePackage(*FixedPackageName);

	ResultMesh = MergeSkeletons(mergeParams, GetSkeletalNodesByClass(ActorClass));

	if (!ResultMesh)
	{
		UEditorDialogLibrary::ShowMessage(FText::FromString("Skel Merging"), FText::FromString("Merge Failed!"), EAppMsgType::Ok);
		return false;
	}

	ResultMesh->Rename(*fileName, Package, REN_DontCreateRedirectors);
	ResultMesh->ClearFlags(RF_Transient);
	ResultMesh->SetFlags(RF_Public | RF_Standalone);

	ResultMesh->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(ResultMesh);
	FSavePackageArgs args;
	args.TopLevelFlags = RF_Public | RF_Standalone;
	
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(FixedPackageName, FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, ResultMesh, *PackageFileName, args);
}

bool UJrSkeletalMergingLibrary::SaveMergeMeshes(const FSkeletalMeshMergeParams& mergeParams, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath, USkeletalMesh* &ResultMesh)
{
	const FString AssetPath = FPaths::ProjectContentDir();
	const FString PackagePath = AbsolutePath + fileName;

	FString FixedPackageName;

	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, FixedPackageName))
	{
		UEditorDialogLibrary::ShowMessage(FText::FromString("Skel Merging"), FText::FromString("Invalid export path!"), EAppMsgType::Ok);
		return false;
	}
    
	if (FPackageName::DoesPackageExist(FixedPackageName))
	{
		FixedPackageName += "_New";
	}

	UPackage* Package = CreatePackage(*FixedPackageName);

	ResultMesh = MergeMeshes(mergeParams);

	if (!ResultMesh)
	{
		UEditorDialogLibrary::ShowMessage(FText::FromString("Skel Merging"), FText::FromString("Merge Failed!"), EAppMsgType::Ok);
		return false;
	}

	if (mergeParams.Skeleton && mergeParams.bSkeletonBefore)
	{
		ResultMesh->SetSkeleton(mergeParams.Skeleton);
	}
	else
	{
		ResultMesh->SetSkeleton(mergeParams.MeshesToMerge[0]->GetSkeleton());
	}

	GenerateImportedModel(ResultMesh);

	ResultMesh->Rename(*fileName, Package, REN_DontCreateRedirectors);
	ResultMesh->ClearFlags(RF_Transient);
	ResultMesh->SetFlags(RF_Public | RF_Standalone);
	ResultMesh->CalculateExtendedBounds();
	ResultMesh->CreateBodySetup();

#if WITH_EDITOR
	FSkinnedAssetCompilingManager& Manager = FSkinnedAssetCompilingManager::Get();
	if (Manager.IsAsyncCompilationAllowed(ResultMesh))
	{
		Manager.FinishCompilation({const_cast<USkeletalMesh*>(ResultMesh)});
	}
#endif

	Package->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(ResultMesh);
	FSavePackageArgs args;
	args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(FixedPackageName, FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, ResultMesh, *PackageFileName, args);
}

void UJrSkeletalMergingLibrary::CreateComponentsByNode(USCS_Node* RootNode, UBlueprint* NewBlueprint)
{
	TArray<UActorComponent*> Components;
	
	TArray<USCS_Node*> ChildNodes = RootNode->GetChildNodes();
	
	TArray<USCS_Node*> ChildChildNodes;
	
	for	(auto ChildNode : ChildNodes)
	{
		if (ChildNode->GetChildNodes().Num() == 0)
		{
			Components.Add(ChildNode->ComponentTemplate);
		}
		else
		{
			ChildChildNodes.Add(ChildNode);
		}
	}
	
	FKismetEditorUtilities::FAddComponentsToBlueprintParams Params;
	Params.HarvestMode = FKismetEditorUtilities::EAddComponentToBPHarvestMode::None;
	Params.OptionalNewRootNode = RootNode;
	Params.bKeepMobility = false;
	FKismetEditorUtilities::AddComponentsToBlueprint(NewBlueprint, Components, Params);

	for (const auto ChildNode : ChildChildNodes)
	{
		CreateComponentsByNode(ChildNode, NewBlueprint);
	}
}

bool UJrSkeletalMergingLibrary::CreateBlueprintAssetAfterMerging(USkeletalMesh* SkelMesh, TArray<USkeletalMesh*> ChildSkelMesh, TSubclassOf<AActor> ActorClass, const FString& fileName, const FString& AbsolutePath)
{
	if (!SkelMesh)
	{
		return false;
	}

	const FString PackagePath = AbsolutePath + fileName;

	FString FixedPackageName;

	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, FixedPackageName))
	{
		UEditorDialogLibrary::ShowMessage(FText::FromString("Skel Merging"), FText::FromString("Invalid export path!"), EAppMsgType::Ok);
		return false;
	}
    
	if (FPackageName::DoesPackageExist(FixedPackageName))
	{
		FixedPackageName += "_New";
	}

	UPackage* Package = CreatePackage(*FixedPackageName);

	UClass* BPClass = AActor::StaticClass();
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(BPClass, Package, FName(fileName), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

	NewBlueprint->Rename(*fileName, Package, REN_DontCreateRedirectors);
	NewBlueprint->ClearFlags(RF_Transient);
	NewBlueprint->SetFlags(RF_Public | RF_Standalone);

	Package->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(NewBlueprint);
	FSavePackageArgs args;
	args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(FixedPackageName, FPackageName::GetAssetPackageExtension());

	// 所有的 USceneComponent Node
	TArray<USCS_Node*> NeedAddNodes = GetNodesByClass<USceneComponent>(ActorClass);

	// 原蓝图中合并后的Mesh Node
	TArray<USCS_Node*> MergeMeshNodes;

	for (auto Node : NeedAddNodes)
	{
		if (Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
		{
			const auto SkelMeshComponent = Cast<USkeletalMeshComponent>(Node->ComponentTemplate);

			// ChildSkelMesh 是不需要合并的Mesh
			// 在 ChildSkelMesh 中找到 Mesh, 表示这个Mesh为不需要合并的; 反之为已经合并的Mesh
			// 这里将已经合并的Mesh区分出来
			if (ChildSkelMesh.Find(SkelMeshComponent->GetSkeletalMeshAsset()) == INDEX_NONE)
			{
				MergeMeshNodes.Add(Node);
			}
		}
	}

	for (auto MergeMeshNode : MergeMeshNodes)
	{
		NeedAddNodes.Remove(MergeMeshNode);
	}

	{
		TArray<UActorComponent*> Components;
		auto RootSkelMesh = NewObject<USkeletalMeshComponent>();
		RootSkelMesh->SetSkeletalMeshAsset(SkelMesh);
		
		Components.Add(RootSkelMesh);
		FKismetEditorUtilities::AddComponentsToBlueprint(NewBlueprint, Components);

		Components.Reset();
		Components.Add(NeedAddNodes[0]->ComponentTemplate);
		FKismetEditorUtilities::FAddComponentsToBlueprintParams Params;
		Params.HarvestMode = FKismetEditorUtilities::EAddComponentToBPHarvestMode::None;
		Params.OptionalNewRootNode = NewBlueprint->SimpleConstructionScript->GetDefaultSceneRootNode();
		Params.bKeepMobility = false;
		FKismetEditorUtilities::AddComponentsToBlueprint(NewBlueprint, Components, Params);
		
		CreateComponentsByNode(NeedAddNodes[0], NewBlueprint);
	}

	return UPackage::SavePackage(Package, NewBlueprint, *PackageFileName, args);
}

TArray<USCS_Node*> UJrSkeletalMergingLibrary::GetSkeletalNodesByClass(const TSubclassOf<AActor> ActorClass)
{
	TArray<USCS_Node*> SkeletalNodes;

	if (const TSubclassOf<AActor> ParentClass = Cast<UBlueprintGeneratedClass>(ActorClass->GetSuperStruct()))
	{
		SkeletalNodes.Append(GetSkeletalNodesByClass(ParentClass));
	}
	
	if (const UBlueprintGeneratedClass* ActorBlueprintGeneratedClass = Cast<UBlueprintGeneratedClass>(ActorClass))
	{
		const TArray<USCS_Node*>& ActorBlueprintNodes = ActorBlueprintGeneratedClass->SimpleConstructionScript->GetAllNodes();
		for(USCS_Node* Node : ActorBlueprintNodes)
		{
			if(Node->ComponentClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
			{
				SkeletalNodes.Add(Node);
			}
		}
	}
	
	return SkeletalNodes;
}

void UJrSkeletalMergingLibrary::BoneNameCheck(TArray<USkeleton*> Skeletons)
{
	TArray<USkeleton*> SkeletonsCache;
	for (auto SourceSkeleton : Skeletons)
	{
		bool bRename = false;
		TArray<FMeshBoneInfo> CurrentBoneInfo = SourceSkeleton->GetReferenceSkeleton().GetRawRefBoneInfo();

		if (SkeletonsCache.Num() > 0)
		{
			for (auto Skeleton : SkeletonsCache)
			{
				auto LastBoneInfo = Skeleton->GetReferenceSkeleton().GetRawRefBoneInfo();
				for (auto& BoneInfo : CurrentBoneInfo)
				{
					auto Index = LastBoneInfo.Find(BoneInfo);
					if (Index != INDEX_NONE)
					{
						// 找到同名骨骼
						BoneInfo.Name = FName(FString(BoneInfo.Name.ToString()).Append("_" + SourceSkeleton->GetName()));
						BoneInfo.ExportName = BoneInfo.Name.ToString();
						bRename = true;
					}
				}
			}
		}

		SkeletonsCache.Add(SourceSkeleton);

		// 如果有相同的骨骼名，则保存所有引用该 骨架/骨骼网格体 的资产
		if (bRename)
		{
			IAssetRegistry& AssetRegistry = UAssetManager::Get().GetAssetRegistry();
			FString LPackageName;
			FString RPackageName;
			TArray<FName> OutReferencersName;
			UKismetSystemLibrary::Conv_SoftObjRefToSoftObjPath(SourceSkeleton).ToString().Split(TEXT("."), &LPackageName, &RPackageName);
			AssetRegistry.GetReferencers(FName(LPackageName), OutReferencersName);

			for (auto RefAssetName : OutReferencersName)
			{
				auto Asset = UEditorAssetLibrary::LoadAsset(RefAssetName.ToString());
				auto SkelMeshAsset = Cast<USkeletalMesh>(Asset);

				if (SkelMeshAsset)
				{
					USkeleton* Skeleton = SkelMeshAsset->GetSkeleton();
					FReferenceSkeleton& SkeletonRef = SkelMeshAsset->GetRefSkeleton();
					TArray<FMeshBoneInfo> CopyBoneInfo = SkeletonRef.GetRawRefBoneInfo();
					TArray<FTransform> CopyBonePose = SkeletonRef.GetRawRefBonePose();
					
					SkeletonRef.Empty();
					FReferenceSkeletonModifier SourceMeshModifier(SkeletonRef, Skeleton);

					for (int32 i = 0; i < CopyBoneInfo.Num(); ++i)
					{
						CopyBoneInfo[i] = CurrentBoneInfo[i];
						SourceMeshModifier.Add(CopyBoneInfo[i], CopyBonePose[i]);
					}

					SkelMeshAsset->MarkPackageDirty();

					// 保存Mesh资产
					UPackage* Package = SkelMeshAsset->GetPackage();
					const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetPathName(), FPackageName::GetAssetPackageExtension());
					FSavePackageArgs args;
					args.TopLevelFlags = RF_Public | RF_Standalone;
					UPackage::SavePackage(Package, SkelMeshAsset, *PackageFileName, args);
				}
			}

			for (auto RefAssetName : OutReferencersName)
			{
				auto Asset = UEditorAssetLibrary::LoadAsset(RefAssetName.ToString());
				auto SkelMeshAsset = Cast<USkeletalMesh>(Asset);
				if (SkelMeshAsset)
				{
					auto Obj = UKismetSystemLibrary::LoadAsset_Blocking(SourceSkeleton);
					auto Skeleton = Cast<USkeleton>(Obj);
					if (Skeleton)
					{
						Skeleton->RecreateBoneTree(SkelMeshAsset);
						Skeleton->MarkPackageDirty();

						// 保存骨架资产
						UPackage* Package = Skeleton->GetPackage();
						const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetPathName(), FPackageName::GetAssetPackageExtension());
						FSavePackageArgs args;
						args.TopLevelFlags = RF_Public | RF_Standalone;
						UPackage::SavePackage(Package, Skeleton, *PackageFileName, args);

						break;
					}
				}
			}
		}
	}
}

void UJrSkeletalMergingLibrary::ModifySameBoneName(TArray<TObjectPtr<USkeleton>>& Skeletons)
{
	TObjectPtr<USkeleton> RootSkeleton = nullptr;
	for (TObjectPtr<USkeleton>& SourceSkeleton : Skeletons)
	{
		if (RootSkeleton)
		{
			TArray<FMeshBoneInfo> CurrentBoneInfo = SourceSkeleton->GetReferenceSkeleton().GetRawRefBoneInfo();
			auto LastBoneInfo = SourceSkeleton->GetReferenceSkeleton().GetRawRefBoneInfo();
			for (auto& BoneInfo : CurrentBoneInfo)
			{
				auto Index = LastBoneInfo.Find(BoneInfo);
				if (Index != INDEX_NONE)
				{
					// 找到同名骨骼
					BoneInfo.Name = FName(FString(BoneInfo.Name.ToString()).Append("_" + SourceSkeleton->GetName()));
					BoneInfo.ExportName = BoneInfo.Name.ToString();
				}
			}
		}
		else
		{
			RootSkeleton = SourceSkeleton;
		}
	}
}

void UJrSkeletalMergingLibrary::MergeSkeletal(FSkeletalMeshMergeParams& SkeletalMeshMergeParams, FSkeletonMergeParams& SkeletonMergeParams, TArray<USCS_Node*> SkeletalNodes)
{
	TArray<TObjectPtr<USkeleton>> SkeletonsToMergeCopy = SkeletonMergeParams.SkeletonsToMerge;

	SkeletonsToMergeCopy.RemoveAll([](USkeleton* InSkeleton)
	{
		return InSkeleton == nullptr;
	});

	if (SkeletonsToMergeCopy.Num() <= 1)
	{
		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("Must provide multiple valid Skeletal Meshes in order to perform a merge."));
		return;
	}

	TArray<TObjectPtr<USkeleton>> Skelesons;
	for (auto Skeleton : SkeletonsToMergeCopy)
	{
		USkeleton* CopyMesh = DuplicateObject<USkeleton>(Skeleton, nullptr);
		Skeleton = CopyMesh;
	}

	ModifySameBoneName(SkeletonsToMergeCopy);
	// USkeleton* Skeleton = MergeSkeletons(SkeletonMergeParams, SkeletalNodes);
	// if (SkeletalMeshMergeParams.bSkeletonBefore)
	// {
	// 	SkeletalMeshMergeParams.Skeleton = Skeleton;	
	// }
	// MergeMeshes(SkeletalMeshMergeParams);
}

USkeleton* UJrSkeletalMergingLibrary::MergeSkeletons(const FSkeletonMergeParams& Params, TArray<USCS_Node*> SkeletalNodes)
{
	// List of unique skeletons generated from input parameters
	TArray<TObjectPtr<USkeleton>> ToMergeSkeletons;
	for ( TObjectPtr<USkeleton> SkeletonPtr : Params.SkeletonsToMerge)
	{
		ToMergeSkeletons.AddUnique(SkeletonPtr);
	}

	// Ensure we have at least one valid Skeleton to merge
	const int32 NumberOfSkeletons = ToMergeSkeletons.Num();
	if (NumberOfSkeletons == 0)
	{
		return nullptr;
	}

	// Calculate potential total number of bones, used for pre-allocating data arrays
	const int32 TotalPossibleBones = Algo::Accumulate(ToMergeSkeletons, 0, [](int32 Sum, const USkeleton* Skeleton)
	{
		return Sum + Skeleton->GetReferenceSkeleton().GetRawBoneNum();
	});

	// Ensure a valid skeleton (number of bones) will be generated
	if (TotalPossibleBones == 0)
	{
		return nullptr;
	}

	UE::SkeletonMerging::FMergedBoneHierarchy MergedBoneHierarchy(TotalPossibleBones);
	
	// Accumulated hierarchy hash from parent-bone to root bone
	TMap<FName, uint32> BoneNamesToPathHash;
	BoneNamesToPathHash.Reserve(TotalPossibleBones);

	// Bone name to bone pose 
	TMap<FName, FTransform> BoneNamesToBonePose;
	BoneNamesToBonePose.Reserve(TotalPossibleBones);

	// Combined bone and socket name hash
	TMap<uint32, TObjectPtr<USkeletalMeshSocket>> HashToSockets;
	// Combined from and to-bone name hash
	TMap<uint32, const FVirtualBone*> HashToVirtualBones;

	TMap<FName, const FCurveMetaData*> UniqueCurveNames;
	TMap<FName, TSet<FName>> GroupToSlotNames;
	TMap<FName, TArray<const UBlendProfile*>> UniqueBlendProfiles;

	bool bMergeSkeletonsFailed = false;

	for (int32 SkeletonIndex = 0; SkeletonIndex < NumberOfSkeletons; ++SkeletonIndex)
	{
		const USkeleton* Skeleton = ToMergeSkeletons[SkeletonIndex];
		const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
		const TArray<FMeshBoneInfo>& Bones = ReferenceSkeleton.GetRawRefBoneInfo();
		const TArray<FTransform>& BonePoses = ReferenceSkeleton.GetRawRefBonePose();

		bool bConflictivePoseFound = false;

		const int32 NumBones = Bones.Num();
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FMeshBoneInfo& Bone = Bones[BoneIndex];

			// Retrieve parent bone name and respective hash, root-bone is assumed to have a parent hash of 0
			FName ParentName = Bone.ParentIndex != INDEX_NONE ? Bones[Bone.ParentIndex].Name : NAME_None;
			ParentName = FName(ParentName.ToString() / FName("1").ToString());
			uint32 ParentHash = Bone.ParentIndex != INDEX_NONE ? GetTypeHash(ParentName) : 0;

			USCS_Node* BoneNode;
			if (SkeletonIndex > 0 && Bone.ParentIndex == INDEX_NONE)
			{
				// 修改 ParentName 为蓝图里的 AttachSocket, 如果蓝图的 Socket 为空, 则修改为父Mesh组件的Root骨骼名
				for (USCS_Node* Node : SkeletalNodes)
				{
					if (USkeleton* InSkeleton = Cast<USkeletalMeshComponent>(Node->ComponentTemplate)->GetSkeletalMeshAsset()->GetSkeleton(); InSkeleton == Skeleton)
					{
						BoneNode = Node;
						// ParentName = Node->AttachToName;
						ParentName = FName(Node->AttachToName.ToString() / "1");
						
						if (ParentName.IsNone())
						{
							for (USCS_Node* Node1 : SkeletalNodes)
							{
								bool Find = false;
								for (auto ChildNode : Node1->GetChildNodes())
								{
									if (ChildNode->ComponentTemplate == Node->ComponentTemplate)
									{
										Find = true;
										ParentName = Cast<USkeletalMeshComponent>(Node1->ComponentTemplate)->GetSkeletalMeshAsset()->GetRefSkeleton().GetRawRefBoneInfo()[0].Name;
										break;
									}
								}
								
								if (Find)
								{
									break;
								}
							}
						}
						
						ParentHash = GetTypeHash(ParentName);
						break;
					}
				}
			}
			
			// FName Bone_Name(Bone.Name);
			FName Bone_Name(Bone.Name.ToString() / "1");
			
			// Look-up the path-hash from root to the parent bone
			const uint32* ParentPath = BoneNamesToPathHash.Find(ParentName);
			const uint32 ParentPathHash = ParentPath ? *ParentPath : 0;

			// Append parent hash to path to give full path hash to current bone
			const uint32 BonePathHash = HashCombine(ParentPathHash, ParentHash);

			if (Params.bCheckSkeletonsCompatibility)
			{
				// Check if the bone exists in the hierarchy 
				if (const uint32* ExistingPath = BoneNamesToPathHash.Find(Bone_Name))
				{
					const uint32 ExistingPathHash = *ExistingPath;

					// If the hash differs from the existing one it means skeletons are incompatible
					if (ExistingPathHash != BonePathHash)
					{
						UE_LOG(LogSkeletalMeshMerge, Error, TEXT("Failed to merge skeletons. Skeleton %s has an invalid bone chain."), *Skeleton->GetName());
						bMergeSkeletonsFailed = true;
						break;
					}

					// Bone poses will be overwritten, check if they are the same
					if (!bConflictivePoseFound && !BoneNamesToBonePose[Bone_Name].Equals(BonePoses[BoneIndex]))
					{
						UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("Skeleton %s has a different reference pose, reference pose will be overwritten."), *Skeleton->GetName());
						bConflictivePoseFound = true;
					}
				}

				BoneNamesToBonePose.Add(Bone_Name, BonePoses[BoneIndex]);
			}
			
			// Add path hash to current bone
			BoneNamesToPathHash.Add(Bone_Name, BonePathHash);

			// Add bone to hierarchy
			FTransform Transform = BonePoses[BoneIndex];

			if (SkeletonIndex > 0)
			{
				USkeletalMeshComponent* SkelMeshComponent = Cast<USkeletalMeshComponent>(BoneNode->ComponentTemplate);
				USkeletalMesh* SkelMesh = SkelMeshComponent->GetSkeletalMeshAsset();
				FReferenceSkeleton RefSkeleton = SkelMesh->GetRefSkeleton();
				TArray<FTransform> RefPose = RefSkeleton.GetRawRefBonePose();

				// 网格体资产里骨骼的偏移
				const uint32 Index = RefSkeleton.FindBoneIndex(Bone_Name);
				if (RefPose.IsValidIndex(Index))
				{
					FTransform BoneRelativeTransform = RefPose[Index];

					if (Bone.ParentIndex == INDEX_NONE)
					{
						// 部件Mesh在蓝图资产里相对于Socket的坐标
						FTransform MeshRelativeTransform = SkelMeshComponent->GetRelativeTransform();
						// 部件Mesh在本地资产坐标系下的Transform, 也就是部件资产Root骨骼的Transform
						FTransform BoneWorldTransform = SkelMeshComponent->GetSkinnedAsset()->GetRefSkeleton().GetRawRefBonePose()[0];
						Transform.SetLocation(MeshRelativeTransform.TransformPosition(BoneWorldTransform.GetLocation()));
						Transform.SetRotation(MeshRelativeTransform.TransformRotation(BoneWorldTransform.GetRotation()));
					}
					else if (BoneIndex > 0)
					{
						Transform.SetLocation(BoneRelativeTransform.GetLocation());
						Transform.SetRotation(BoneRelativeTransform.GetRotation());
					}
				}
			}

			MergedBoneHierarchy.AddBone(Bone_Name, Transform, BonePathHash);
		}

		if (Params.bCheckSkeletonsCompatibility && bMergeSkeletonsFailed)
		{
			continue;
		}

		if (Params.bMergeSockets)
		{
			for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
			{
				const uint32 Hash = HashCombine(GetTypeHash(Socket->SocketName), GetTypeHash(Socket->BoneName));
				HashToSockets.Add(Hash, Socket);
			}
		}

		if (Params.bMergeVirtualBones)
		{
			const TArray<FVirtualBone>& VirtualBones = Skeleton->GetVirtualBones();		
	        for (const FVirtualBone& VB : VirtualBones)
	        {
	            const uint32 Hash = HashCombine(GetTypeHash(VB.SourceBoneName), GetTypeHash(VB.TargetBoneName));
	            HashToVirtualBones.Add(Hash, &VB);
	        }
		}		

		if (Params.bMergeCurveNames)
		{
			if (const FSmartNameMapping* CurveMappingPtr = Skeleton->GetSmartNameContainer(USkeleton::AnimCurveMappingName))
			{
				TArray<FName> CurveNames;
				CurveMappingPtr->FillNameArray(CurveNames);
				for (const FName& CurveName : CurveNames)
				{
					UniqueCurveNames.FindOrAdd(CurveName) = CurveMappingPtr->GetCurveMetaData(CurveName);
				}
			}
		}

		if (Params.bMergeAnimSlotGroups)
		{
			const TArray<FAnimSlotGroup>& SlotGroups = Skeleton->GetSlotGroups();
			for (const FAnimSlotGroup& AnimSlotGroup : SlotGroups)
			{
				GroupToSlotNames.FindOrAdd(AnimSlotGroup.GroupName).Append(AnimSlotGroup.SlotNames);
			}
		}
		
		if (Params.bMergeBlendProfiles)
		{
			for ( const TObjectPtr<UBlendProfile>& BlendProfile : Skeleton->BlendProfiles)
			{
				UniqueBlendProfiles.FindOrAdd(BlendProfile->GetFName()).Add(BlendProfile.Get());
			}			
		}		
	}

	if (bMergeSkeletonsFailed)
	{
		UE_LOG(LogSkeletalMeshMerge, Error, TEXT("Failed to merge skeletons. One or more skeletons with invalid parent chains were found."));
		return nullptr;
	}

	USkeleton* GeneratedSkeleton = NewObject<USkeleton>();

	// Generate bone hierarchy
	{		
		FReferenceSkeletonModifier Modifier(GeneratedSkeleton);
		MergedBoneHierarchy.PopulateSkeleton(Modifier);
	}
	
	// Merge sockets
	if (Params.bMergeSockets)
	{
		TArray<TObjectPtr<USkeletalMeshSocket>> Sockets;
		HashToSockets.GenerateValueArray(Sockets);
		AddSockets(GeneratedSkeleton, Sockets);
	}
	
	// Merge virtual bones
	if (Params.bMergeVirtualBones)
	{
		TArray<const FVirtualBone*> VirtualBones;
		HashToVirtualBones.GenerateValueArray(VirtualBones);
		AddVirtualBones(GeneratedSkeleton,VirtualBones);
	}

	// Merge Curve / track mappings	
	if (Params.bMergeCurveNames)
	{
		AddCurveNames(GeneratedSkeleton, UniqueCurveNames);
	}

	// Merge blend profiles
	if (Params.bMergeBlendProfiles)
	{
		AddBlendProfiles(GeneratedSkeleton, UniqueBlendProfiles);
	}

	// Merge SlotGroups
	if (Params.bMergeAnimSlotGroups)
	{
		AddAnimationSlotGroups(GeneratedSkeleton, GroupToSlotNames);
	}
		
	return GeneratedSkeleton;
}

USkeletalMesh* UJrSkeletalMergingLibrary::MergeMeshes(const FSkeletalMeshMergeParams& Params)
{
	TArray<USkeletalMesh*> MeshesToMergeCopy = Params.MeshesToMerge;

	MeshesToMergeCopy.RemoveAll([](USkeletalMesh* InMesh)
	{
		return InMesh == nullptr;
	});

	if (MeshesToMergeCopy.Num() <= 1)
	{
		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("Must provide multiple valid Skeletal Meshes in order to perform a merge."));
		return nullptr;
	}

	EMeshBufferAccess BufferAccess = Params.bNeedsCpuAccess ?
										EMeshBufferAccess::ForceCPUAndGPU :
										EMeshBufferAccess::Default;
	
	bool bRunDuplicateCheck = false;
	USkeletalMesh* BaseMesh = NewObject<USkeletalMesh>();

	// 拷贝一个新的Mesh替换掉第一个Mesh
	USkeletalMesh* CopyMesh = DuplicateObject<USkeletalMesh>(MeshesToMergeCopy[0], nullptr);
	MeshesToMergeCopy[0] = CopyMesh;

	if (Params.Skeleton && Params.bSkeletonBefore)
	{
		USkeleton* NewSkeleton = Params.Skeleton;
		BaseMesh->SetSkeleton(NewSkeleton);

		// 默认第一个元素为主体， 把新骨架赋值给主体， 后续会拿主体的骨架进行计算
		USkeletalMesh* RootSkelMesh = MeshesToMergeCopy[0];

		FReferenceSkeleton NewRefSkeleton = NewSkeleton->GetReferenceSkeleton();
		
		FReferenceSkeletonModifier Modifier(RootSkelMesh->GetRefSkeleton(), NewSkeleton);

		// 已经处理过的骨架，下方添加骨骼信息时不会对这些骨骼进行操作，因为这些骨架已经添加过了
		TArray<USkeleton*> MergedSkeletons;
		for (int MeshIndex = 1; MeshIndex < MeshesToMergeCopy.Num(); MeshIndex++)
		{
			USkeletalMesh* Mesh = MeshesToMergeCopy[MeshIndex];
			USkeleton* Skeleton = Mesh->GetSkeleton();
			bool SkipProcess = false;
			if (Skeleton == RootSkelMesh->GetSkeleton())
			{
				SkipProcess = true;
			}
			else
			{
				for (auto MergedSkeleton : MergedSkeletons)
				{
					if (Skeleton == MergedSkeleton)
					{
						SkipProcess = true;
						break;
					}
				}
			}

			// 之前已经添加过, 跳过添加骨骼信息
			if (SkipProcess)
			{
				continue;
			}

			MergedSkeletons.AddUnique(Skeleton);

			TArray<FMeshBoneInfo> BoneInfoArr = Mesh->GetRefSkeleton().GetRawRefBoneInfo();
			for (int BoneInfoIndex = 0; BoneInfoIndex < BoneInfoArr.Num(); BoneInfoIndex++)
			{
				FMeshBoneInfo BoneInfo = BoneInfoArr[BoneInfoIndex];
				int32 BoneIdxOnNewSkeleton = NewRefSkeleton.FindRawBoneIndex(BoneInfo.Name);
				FTransform BonePose = NewRefSkeleton.GetRawRefBonePose()[BoneIdxOnNewSkeleton];

				if (BoneInfoIndex == 0)
				{
					// 不同骨架合并的时候， 将Root骨骼的ParentIndex设置为美术蓝图里Socket的骨骼
					FName ParentName = NewRefSkeleton.GetBoneName(NewRefSkeleton.GetRawParentIndex(BoneIdxOnNewSkeleton));
					BoneInfo.ParentIndex = RootSkelMesh->GetRefSkeleton().FindBoneIndex(ParentName);
				}
				else
				{
					// 不同骨架合并的时候， 将非Root骨骼的ParentIndex设置为Root骨骼合并后的Index
					BoneInfo.ParentIndex = RootSkelMesh->GetRefSkeleton().FindRawBoneIndex(BoneInfoArr[BoneInfo.ParentIndex].Name);
				}

				// 以第一个Mesh为基础， 将其他不同骨架的Mesh骨骼信息添加到第一个Mesh
				Modifier.Add(BoneInfo, BonePose);
			}
		}

		RootSkelMesh->SetSkeleton(NewSkeleton);

		bRunDuplicateCheck = true;
	}

	FSkelMeshMergeUVTransformMapping Mapping;
	Mapping.UVTransformsPerMesh = Params.UVTransformsPerMesh;
	FJrSkeletalMeshMerge Merger(BaseMesh, MeshesToMergeCopy, Params.MeshSectionMappings, Params.StripTopLODS, BufferAccess, &Mapping);
	if (!Merger.DoMerge())
	{
		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("Merge failed!"));
		return nullptr;
	}
	
	// 获取骨骼网格体的 LOD0 顶点数据
	const FSkeletalMeshLODRenderData& LODData = BaseMesh->GetResourceForRendering()->LODRenderData[0];
	const FPositionVertexBuffer& VertexBuffer = LODData.StaticVertexBuffers.PositionVertexBuffer;

	// 初始化最小和最大坐标为第一个顶点的坐标
	FVector3f MinVertex = VertexBuffer.VertexPosition(0);
	FVector3f MaxVertex = VertexBuffer.VertexPosition(0);
	
	for (uint32 VertexIndex = 0; VertexIndex < VertexBuffer.GetNumVertices(); ++VertexIndex )
	{
		const FVector3f& Vertex = VertexBuffer.VertexPosition(VertexIndex);
		MinVertex = FVector3f::Min(MinVertex, Vertex);
		MaxVertex = FVector3f::Max(MaxVertex, Vertex);
	}
	
	FBox BoxBounds(MinVertex, MaxVertex);
	BaseMesh->SetImportedBounds(FBoxSphereBounds(BoxBounds));
	
	if (Params.Skeleton && !Params.bSkeletonBefore)
	{
		BaseMesh->SetSkeleton(Params.Skeleton);
	}
	
	if (bRunDuplicateCheck)
	{
		TArray<FName> SkelMeshSockets;
		TArray<FName> SkelSockets;

		for (USkeletalMeshSocket* Socket : BaseMesh->GetMeshOnlySocketList())
		{
			if (Socket)
			{
				SkelMeshSockets.Add(Socket->GetFName());
			}
		}

		for (USkeletalMeshSocket* Socket : BaseMesh->GetSkeleton()->Sockets)
		{
			if (Socket)
			{
				SkelSockets.Add(Socket->GetFName());
			}
		}

		TSet<FName> UniqueSkelMeshSockets;
		TSet<FName> UniqueSkelSockets;
		
		UniqueSkelMeshSockets.Append(SkelMeshSockets);
		UniqueSkelSockets.Append(SkelSockets);

		int32 Total = SkelSockets.Num() + SkelMeshSockets.Num();
		int32 UniqueTotal = UniqueSkelMeshSockets.Num() + UniqueSkelSockets.Num();

		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("SkelMeshSocketCount: %d | SkelSocketCount: %d | Combined: %d"), SkelMeshSockets.Num(), SkelSockets.Num(), Total);
		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("SkelMeshSocketCount: %d | SkelSocketCount: %d | Combined: %d"), UniqueSkelMeshSockets.Num(), UniqueSkelSockets.Num(), UniqueTotal);
		UE_LOG(LogSkeletalMeshMerge, Warning, TEXT("Found Duplicates: %s"), *((Total != UniqueTotal) ? FString("True") : FString("False")));
	}

	return BaseMesh;
}

TArray<USkeletalMeshComponent*> UJrSkeletalMergingLibrary::GetSkeletalMeshByClass(const TSubclassOf<AActor> ActorClass)
{
	TArray<USkeletalMeshComponent*> SkelMeshes;

	bool HasRoot = false;
	
	for(const USCS_Node* Node : GetSkeletalNodesByClass(ActorClass))
	{
		if (Node->ComponentTemplate)
		{
			if (USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(Node->ComponentTemplate))
			{
				if (!HasRoot && Node->IsRootNode())
				{
					HasRoot = true;
					
					SkelMeshes.Add(SkeletalMeshComponent);
					
					// 规定主骨架必须要是第一个元素
					SkelMeshes.Swap(0,SkelMeshes.Num() - 1);
				}
				else
				{
					SkelMeshes.Add(SkeletalMeshComponent);
				}
			}
		}
	}

	return SkelMeshes;
}

UObject* UJrSkeletalMergingLibrary::CreateAsset(UObject* Obj, const FString& fileName, const FString& AbsolutePath)
{
	if (!Obj)
	{
		return nullptr;
	}

	const FString PackagePath = AbsolutePath + fileName;

	FString FixedPackageName;

	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, FixedPackageName))
	{
		return nullptr;
	}

	if (!FPackageName::IsValidObjectPath(FixedPackageName))
	{
		return nullptr;
	}
    
	if (FPackageName::DoesPackageExist(FixedPackageName))
	{
		FixedPackageName += "_New";
	}

	UPackage* Package = CreatePackage(*FixedPackageName);

	UObject* NewObj = DuplicateObject<UObject>(Obj, nullptr);
	NewObj->Rename(*fileName, Package, REN_DontCreateRedirectors);
	NewObj->ClearFlags(RF_Transient);
	NewObj->SetFlags(RF_Public | RF_Standalone);

	Package->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(NewObj);
	FSavePackageArgs args;
	args.TopLevelFlags = RF_Public | RF_Standalone;

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(FixedPackageName, FPackageName::GetAssetPackageExtension());
	return UPackage::SavePackage(Package, NewObj, *PackageFileName, args) ? NewObj : nullptr;
	
}

void UJrSkeletalMergingLibrary::SaveAssetsOfClass(UClass* AssetClass)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	// 构建一个类过滤器
	FARFilter AssetFilter;
	AssetFilter.ClassPaths.Add(AssetClass->GetClassPathName());

	// 获取资产数据
	TArray<FAssetData> AssetData;
	AssetRegistryModule.Get().GetAssets(AssetFilter, AssetData);

	for (auto Data : AssetData)
	{
		UObject* Asset = Data.GetAsset();
		UPackage* Package = Asset->GetPackage();

		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs args;
		args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Asset, *PackageFileName, args);
	}
}

UE_DISABLE_OPTIMIZATION
FMeshBuildSettings UJrSkeletalMergingLibrary::GetBuildSettingsFromStaticMesh(UStaticMesh* StaticMesh, const int32 LODIndex)
{
	return StaticMesh && StaticMesh->GetSourceModels().IsValidIndex(LODIndex) ? StaticMesh->GetSourceModel(LODIndex).BuildSettings : FMeshBuildSettings();
}

UMaterialInstanceConstant* UJrSkeletalMergingLibrary::CreateMIC_EditorOnly(UMaterialInterface* Material, FString InName)
{
#if WITH_EDITOR
	TArray<UObject*> ObjectsToSync;

	if (Material != nullptr)
	{
		// Create an appropriate and unique name 
		FString Name;
		FString PackageName;
		IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

		//Use asset name only if directories are specified, otherwise full path
		if (!InName.Contains(TEXT("/")))
		{
			FString AssetName = Material->GetOutermost()->GetName();
			const FString SanitizedBasePackageName = UPackageTools::SanitizePackageName(AssetName);
			const FString PackagePath = FPackageName::GetLongPackagePath(SanitizedBasePackageName) + TEXT("/");
			AssetTools.CreateUniqueAssetName(PackagePath, InName, PackageName, Name);
		}
		else
		{
			AssetTools.CreateUniqueAssetName(InName, TEXT(""), PackageName, Name);
		}

		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Material;

		UObject* NewAsset = AssetTools.CreateAsset(Name, FPackageName::GetLongPackagePath(PackageName), UMaterialInstanceConstant::StaticClass(), Factory);

		ObjectsToSync.Add(NewAsset);
		GEditor->SyncBrowserToObjects(ObjectsToSync);

		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(NewAsset);

		return MIC;
	}
#endif
	return nullptr;
}

void UJrSkeletalMergingLibrary::UpdateMaterialInstanceFromDataAsset(UAnimToTextureDataAsset* DataAsset, UMaterialInstanceConstant* MaterialInstance, 
	const bool bAnimate, const EAnimToTextureNumBoneInfluences NumBoneInfluences, const EMaterialParameterAssociation MaterialParameterAssociation)
{
	if (!MaterialInstance || !DataAsset)
	{
		return;
	}

	FMaterialLayersFunctions OutLayers;
	MaterialInstance->GetMaterialLayers(OutLayers);
	const int32 LayerIndex = OutLayers.Layers.Num() - 1;

	UMaterialFunctionInterface* MaterialFunctionInterface;
	OutLayers.Layers.Add(MaterialFunctionInterface);

	// Set UVChannel
	switch (DataAsset->UVChannel)
	{
		case 0:
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV0, true, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV1, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV2, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV3, false, MaterialParameterAssociation, LayerIndex);
			break;
		case 1:
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV0, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV1, true, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV2, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV3, false, MaterialParameterAssociation, LayerIndex);
			break;
		case 2:
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV0, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV1, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV2, true, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV3, false, MaterialParameterAssociation, LayerIndex);
			break;
		case 3:
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV0, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV1, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV2, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV3, true, MaterialParameterAssociation, LayerIndex);
			break;
		default:
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV0, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV1, true, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV2, false, MaterialParameterAssociation, LayerIndex);
			SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseUV3, false, MaterialParameterAssociation, LayerIndex);
			break;
	}

	// Update Vertex Params
	if (DataAsset->Mode == EAnimToTextureMode::Vertex)
	{
		FLinearColor VectorParameter;
		VectorParameter = FLinearColor(DataAsset->VertexMinBBox);
		SetMaterialInstanceVectorParameterValue(MaterialInstance, AnimToTextureParamNames::BoundingBoxMin, VectorParameter, MaterialParameterAssociation, LayerIndex);
		
		VectorParameter = FLinearColor(DataAsset->VertexSizeBBox);
		SetMaterialInstanceVectorParameterValue(MaterialInstance, AnimToTextureParamNames::BoundingBoxScale, VectorParameter, MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceScalarParameterValue(MaterialInstance, AnimToTextureParamNames::NumFrames, DataAsset->NumFrames, MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceScalarParameterValue(MaterialInstance, AnimToTextureParamNames::RowsPerFrame, DataAsset->VertexRowsPerFrame, MaterialParameterAssociation, LayerIndex);

		SetMaterialInstanceTextureParameterValue(MaterialInstance, AnimToTextureParamNames::VertexPositionTexture, DataAsset->GetVertexPositionTexture(), MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceTextureParameterValue(MaterialInstance, AnimToTextureParamNames::VertexNormalTexture, DataAsset->GetVertexNormalTexture(), MaterialParameterAssociation, LayerIndex);

	}

	// Update Bone Params
	else if (DataAsset->Mode == EAnimToTextureMode::Bone)
	{
		FLinearColor VectorParameter;
		VectorParameter = FLinearColor(DataAsset->BoneMinBBox);
		SetMaterialInstanceVectorParameterValue(MaterialInstance, AnimToTextureParamNames::BoundingBoxMin, VectorParameter, MaterialParameterAssociation, LayerIndex);

		VectorParameter = FLinearColor(DataAsset->BoneSizeBBox);
		SetMaterialInstanceVectorParameterValue(MaterialInstance, AnimToTextureParamNames::BoundingBoxScale, VectorParameter, MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceScalarParameterValue(MaterialInstance, AnimToTextureParamNames::NumFrames, DataAsset->NumFrames, MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceScalarParameterValue(MaterialInstance, AnimToTextureParamNames::RowsPerFrame, DataAsset->BoneRowsPerFrame, MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceScalarParameterValue(MaterialInstance, AnimToTextureParamNames::BoneWeightRowsPerFrame, DataAsset->BoneWeightRowsPerFrame, MaterialParameterAssociation, LayerIndex);

		SetMaterialInstanceTextureParameterValue(MaterialInstance, AnimToTextureParamNames::BonePositionTexture, DataAsset->GetBonePositionTexture(), MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceTextureParameterValue(MaterialInstance, AnimToTextureParamNames::BoneRotationTexture, DataAsset->GetBoneRotationTexture(), MaterialParameterAssociation, LayerIndex);
		SetMaterialInstanceTextureParameterValue(MaterialInstance, AnimToTextureParamNames::BoneWeightsTexture, DataAsset->GetBoneWeightTexture(), MaterialParameterAssociation, LayerIndex);

		// Num Influences
		switch (NumBoneInfluences)
		{
			case EAnimToTextureNumBoneInfluences::One:
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseTwoInfluences, false, MaterialParameterAssociation, LayerIndex);
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseFourInfluences, false, MaterialParameterAssociation, LayerIndex);
				break;
			case EAnimToTextureNumBoneInfluences::Two:
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseTwoInfluences, true, MaterialParameterAssociation, LayerIndex);
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseFourInfluences, false, MaterialParameterAssociation, LayerIndex);
				break;
			case EAnimToTextureNumBoneInfluences::Four:
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseTwoInfluences, false, MaterialParameterAssociation, LayerIndex);
				SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::UseFourInfluences, true, MaterialParameterAssociation, LayerIndex);
				break;
		}

	}

	// Animate
	SetMaterialInstanceStaticSwitchParameterValue(MaterialInstance, AnimToTextureParamNames::Animate, bAnimate, MaterialParameterAssociation, LayerIndex);

	// Update Material
	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);

	// Rebuild Material
	UMaterialEditingLibrary::RebuildMaterialInstanceEditors(MaterialInstance->GetMaterial());

	// Set Preview Mesh
	if (DataAsset->GetStaticMesh())
	{
		MaterialInstance->PreviewMesh = DataAsset->GetStaticMesh();
	}

	MaterialInstance->MarkPackageDirty();
}

float UJrSkeletalMergingLibrary::GetMaterialInstanceScalarParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	float Result = 0.f;
	if (Instance)
	{
		Instance->GetScalarParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Result);
	}
	return Result;
}

bool UJrSkeletalMergingLibrary::SetMaterialInstanceScalarParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, float Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);
	}
	return bResult;
}

UTexture* UJrSkeletalMergingLibrary::GetMaterialInstanceTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	UTexture* Result = nullptr;
	if (Instance)
	{
		Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Result);
	}
	return Result;
}


bool UJrSkeletalMergingLibrary::SetMaterialInstanceTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, UTexture* Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);
	}
	return bResult;
}


URuntimeVirtualTexture* UJrSkeletalMergingLibrary::GetMaterialInstanceRuntimeVirtualTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	URuntimeVirtualTexture* Result = nullptr;
	if (Instance)
	{
		Instance->GetRuntimeVirtualTextureParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Result);
	}
	return Result;
}

bool UJrSkeletalMergingLibrary::SetMaterialInstanceRuntimeVirtualTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, URuntimeVirtualTexture* Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetRuntimeVirtualTextureParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);
	}
	return bResult;
}


USparseVolumeTexture* UJrSkeletalMergingLibrary::GetMaterialInstanceSparseVolumeTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	USparseVolumeTexture* Result = nullptr;
	if (Instance)
	{
		Instance->GetSparseVolumeTextureParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Result);
	}
	return Result;
}

bool UJrSkeletalMergingLibrary::SetMaterialInstanceSparseVolumeTextureParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, USparseVolumeTexture* Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetSparseVolumeTextureParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);
	}
	return bResult;
}


FLinearColor UJrSkeletalMergingLibrary::GetMaterialInstanceVectorParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	FLinearColor Result = FLinearColor::Black;
	if (Instance)
	{
		Instance->GetVectorParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Result);
	}
	return Result;
}

bool UJrSkeletalMergingLibrary::SetMaterialInstanceVectorParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, FLinearColor Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);
	}
	return bResult;
}


bool UJrSkeletalMergingLibrary::GetMaterialInstanceStaticSwitchParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		FGuid OutGuid;
		Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), bResult, OutGuid);
	}
	return bResult;
}

bool UJrSkeletalMergingLibrary::SetMaterialInstanceStaticSwitchParameterValue(UMaterialInstanceConstant* Instance, FName ParameterName, bool Value, EMaterialParameterAssociation Association, int32 LayerIndex)
{
	bool bResult = false;
	if (Instance)
	{
		Instance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(ParameterName, Association, Association == LayerParameter ? LayerIndex : INDEX_NONE), Value);

		// The material instance editor window puts MaterialLayersParameters into our StaticParameters, if we don't do this, our settings could get wiped out on first launch of the material editor.
		// If there's ever a cleaner and more isolated way of populating MaterialLayersParameters, we should do that instead.
		UMaterialEditorInstanceConstant* MaterialEditorInstance = NewObject<UMaterialEditorInstanceConstant>(GetTransientPackage(), NAME_None, RF_Transactional);
		MaterialEditorInstance->SetSourceInstance(Instance);
	}
	return bResult;
}

TArray<UMaterialFunctionInterface*> UJrSkeletalMergingLibrary::GetMaterialLayers(UMaterialInstanceConstant* Instance)
{
	if (Instance)
	{
		FMaterialLayersFunctions OutLayers;
		Instance->GetMaterialLayers(OutLayers);
		

		return OutLayers.Layers;
	}
	else
	{
		return TArray<UMaterialFunctionInterface*>();
	}
}

void UJrSkeletalMergingLibrary::AddMaterialLayer(UMaterialInstanceConstant* Instance, UMaterialFunctionInterface* MaterialLayer, UMaterialFunctionInterface* BlendLayer, bool bVisible, EMaterialLayerLinkState LinkState)
{
	if (!Instance)
	{
		return;
	}

	FMaterialLayersFunctions OutLayers;
	Instance->GetMaterialLayers(OutLayers);
	OutLayers.Layers.Add(MaterialLayer);
	OutLayers.Blends.Add(BlendLayer);
	OutLayers.EditorOnly.LayerStates.Add(bVisible);
	const FText LayerName = FText::Format(LOCTEXT("LayerPrefix", "Layer {0}"), OutLayers.Layers.Num() - 1);
	OutLayers.EditorOnly.LayerNames.Add(LayerName);
	OutLayers.EditorOnly.RestrictToLayerRelatives.Add(false);
	OutLayers.EditorOnly.RestrictToBlendRelatives.Add(false);
	OutLayers.EditorOnly.LayerGuids.Add(FGuid::NewGuid());
	OutLayers.EditorOnly.LayerLinkStates.Add(LinkState);

	Instance->SetMaterialLayers(OutLayers);
	
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

	// 如果资产已经打开, 则重新打开资产以刷新数据
	if (AssetEditorSubsystem->FindEditorForAsset(Instance, false))
	{
		AssetEditorSubsystem->CloseAllEditorsForAsset(Instance);
		AssetEditorSubsystem->OpenEditorForAsset(Instance);
	}
}

bool UJrSkeletalMergingLibrary::UpdateMaterialLayers(UMaterialInstanceConstant* Instance, UMaterialFunctionInterface* MaterialLayer, UMaterialFunctionInterface* BlendLayer, bool bVisible, EMaterialLayerLinkState LinkState)
{
	if (!Instance)
	{
		return false;
	}

	bool Success = true;

	FMaterialLayersFunctions OutLayers;
	Instance->GetMaterialLayers(OutLayers);
	TArray<UMaterialFunctionInterface*> MaterialLayers = OutLayers.Layers;
	
	if (MaterialLayers.Num() > 0)
	{
		const UMaterialFunctionInterface* LastLayer = MaterialLayers.Last();
		if (!LastLayer)
		{

			if (OutLayers.EditorOnly.LayerGuids.Last() == OutLayers.BackgroundGuid)
			{
				OutLayers.Layers.Last() = MaterialLayer;
				OutLayers.EditorOnly.LayerLinkStates.Last() = LinkState;
			}
			else
			{
				OutLayers.Layers.Last() = MaterialLayer;
				OutLayers.Blends.Last() = BlendLayer;
				OutLayers.EditorOnly.LayerStates.Last() = bVisible;
				const FText LayerName = FText::Format(LOCTEXT("LayerPrefix", "Layer {0}"), OutLayers.Layers.Num() - 1);
				OutLayers.EditorOnly.LayerNames.Last() = LayerName;
				OutLayers.EditorOnly.RestrictToLayerRelatives.Last() = false;
				OutLayers.EditorOnly.RestrictToBlendRelatives.Last() = false;
				OutLayers.EditorOnly.LayerGuids.Last() = FGuid::NewGuid();
				OutLayers.EditorOnly.LayerLinkStates.Last() = LinkState;
			}

			Instance->SetMaterialLayers(OutLayers);

			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

			// 如果资产已经打开, 则重新打开资产以刷新数据
			if (AssetEditorSubsystem->FindEditorForAsset(Instance, false))
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Instance);
				AssetEditorSubsystem->OpenEditorForAsset(Instance);
			}
		}
		else if (LastLayer != MaterialLayer)
		{
			AddMaterialLayer(Instance, MaterialLayer, BlendLayer, bVisible, LinkState);
		}
	}
	else
	{
		Success = false;
	}

	return Success;
}

UStaticMesh* UJrSkeletalMergingLibrary::ConvertSkeletalMeshToStaticMesh(USkeletalMesh* SkeletalMesh, const FString PackageName, const int32 LODIndex)
{
	if (!SkeletalMesh || PackageName.IsEmpty())
	{
		return nullptr;
	}

	if (!FPackageName::IsValidObjectPath(PackageName))
	{
		return nullptr;
	}

	if (LODIndex >= 0 && !SkeletalMesh->IsValidLODIndex(LODIndex))
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid LODIndex: %i"), LODIndex);
		return nullptr;
	}

	// Create Temp Actor
	check(GEditor);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	check(World);
	AActor* Actor = World->SpawnActor<AActor>();
	check(Actor);

	// Create Temp SkeletalMesh Component
	USkeletalMeshComponent* MeshComponent = NewObject<USkeletalMeshComponent>(Actor);
	MeshComponent->RegisterComponent();
	MeshComponent->SetSkeletalMesh(SkeletalMesh);
	TArray<UMeshComponent*> MeshComponents = { MeshComponent };

	UStaticMesh* OutStaticMesh = nullptr;
	bool bGeneratedCorrectly = true;

	// Create New StaticMesh
	if (!FPackageName::DoesPackageExist(PackageName))
	{
		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
		OutStaticMesh = MeshUtilities.ConvertMeshesToStaticMesh(MeshComponents, FTransform::Identity, PackageName);
	}
	// Update Existing StaticMesh
	else
	{
		// Load Existing Mesh
		OutStaticMesh = LoadObject<UStaticMesh>(nullptr, *PackageName);
	}

	if (OutStaticMesh)
	{
		// Create Temp Package.
		// because 
		UPackage* TransientPackage = GetTransientPackage();

		// Create Temp Mesh.
		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
		UStaticMesh* TempMesh = MeshUtilities.ConvertMeshesToStaticMesh(MeshComponents, FTransform::Identity, TransientPackage->GetPathName());

		// make sure transactional flag is on
		TempMesh->SetFlags(RF_Transactional);

		// Copy All LODs
		if (LODIndex < 0)
		{
			const int32 NumSourceModels = TempMesh->GetNumSourceModels();
			OutStaticMesh->SetNumSourceModels(NumSourceModels);

			for (int32 Index = 0; Index < NumSourceModels; ++Index)
			{
				// Get RawMesh
				FRawMesh RawMesh;
				TempMesh->GetSourceModel(Index).LoadRawMesh(RawMesh);

				// Set RawMesh
				OutStaticMesh->GetSourceModel(Index).SaveRawMesh(RawMesh);
			};
		}

		// Copy Single LOD
		else
		{
			if (LODIndex >= TempMesh->GetNumSourceModels())
			{
				UE_LOG(LogTemp, Warning, TEXT("Invalid Source Model Index: %i"), LODIndex);
				bGeneratedCorrectly = false;
			}
			else
			{
				const int32 NumSourceModels = OutStaticMesh->GetNumSourceModels();
				for(auto i = NumSourceModels - 1;  i>= 0; --i)
				{
					if (i != LODIndex)
					{
						OutStaticMesh->RemoveSourceModel(i);
					}
				}
				OutStaticMesh->SetNumSourceModels(1);
			}
		}
			
		// Copy Materials
		const TArray<FStaticMaterial>& Materials = TempMesh->GetStaticMaterials();
		OutStaticMesh->SetStaticMaterials(Materials);

		// Done
		TArray<FText> OutErrors;
		OutStaticMesh->Build(true, &OutErrors);
		OutStaticMesh->MarkPackageDirty();
	}

	// Destroy Temp Component and Actor
	MeshComponent->UnregisterComponent();
	MeshComponent->DestroyComponent();
	Actor->Destroy();

	return bGeneratedCorrectly ? OutStaticMesh : nullptr;
}

UE_ENABLE_OPTIMIZATION

void UJrSkeletalMergingLibrary::AddSockets(USkeleton* InSkeleton, const TArray<TObjectPtr<USkeletalMeshSocket>>& InSockets)
{
	for (const TObjectPtr<USkeletalMeshSocket>& MergeSocket : InSockets)
	{
		USkeletalMeshSocket* NewSocket = NewObject<USkeletalMeshSocket>(InSkeleton);
		if (NewSocket != nullptr)
		{
			InSkeleton->Sockets.Add(NewSocket);

			// Copy over all socket information
			NewSocket->SocketName = MergeSocket->SocketName;
			NewSocket->BoneName = MergeSocket->BoneName;
			NewSocket->RelativeLocation = MergeSocket->RelativeLocation;
			NewSocket->RelativeRotation = MergeSocket->RelativeRotation;
			NewSocket->RelativeScale = MergeSocket->RelativeScale;
			NewSocket->bForceAlwaysAnimated = MergeSocket->bForceAlwaysAnimated;
		}
	}
}

void UJrSkeletalMergingLibrary::AddVirtualBones(USkeleton* InSkeleton, const TArray<const FVirtualBone*> InVirtualBones)
{
	for(const FVirtualBone* VirtualBone : InVirtualBones)
	{
		FName VirtualBoneName = NAME_None;				
		InSkeleton->AddNewVirtualBone(VirtualBone->SourceBoneName, VirtualBone->TargetBoneName, VirtualBoneName);
		InSkeleton->RenameVirtualBone(VirtualBoneName, VirtualBone->VirtualBoneName);
	}	
}

void UJrSkeletalMergingLibrary::AddCurveNames(USkeleton* InSkeleton, const TMap<FName, const FCurveMetaData*>& InCurves)
{
	TArray<FSmartName> CurveSmartNames;

	Algo::Transform(InCurves, CurveSmartNames, [](const TPair<FName, const FCurveMetaData*>& CurveMetaDataPair)
	{
		return FSmartName(CurveMetaDataPair.Key, INDEX_NONE);
	});
	InSkeleton->VerifySmartNames(USkeleton::AnimCurveMappingName, CurveSmartNames);
		
	for(const TPair<FName, const FCurveMetaData*>& CurveMetaDataPair : InCurves)
	{
		if (CurveMetaDataPair.Value)
		{
			FCurveMetaData& SkeletonCurveMetaData = *InSkeleton->GetCurveMetaData(CurveMetaDataPair.Key);
			SkeletonCurveMetaData = *CurveMetaDataPair.Value;
			for (FBoneReference& BoneReference : SkeletonCurveMetaData.LinkedBones)
			{
				BoneReference.Initialize(InSkeleton);
			}
		}
	}
}

void UJrSkeletalMergingLibrary::AddBlendProfiles(USkeleton* InSkeleton, const TMap<FName, TArray<const UBlendProfile*>>& InBlendProfiles)
{
	for (const TPair<FName, TArray<const UBlendProfile*>>& BlendProfilesPair : InBlendProfiles)
	{
		const TArray<const UBlendProfile*>& BlendProfiles = BlendProfilesPair.Value;
		UBlendProfile* MergedBlendProfile = InSkeleton->CreateNewBlendProfile(BlendProfilesPair.Key);
			
		for (int32 ProfileIndex = 0; ProfileIndex < BlendProfiles.Num(); ++ProfileIndex)
		{
			const UBlendProfile* Profile = BlendProfiles[ProfileIndex];				
			MergedBlendProfile->Mode = ProfileIndex == 0 ? Profile->Mode : MergedBlendProfile->Mode;

			// Mismatch in terms of blend profile type
			ensure(MergedBlendProfile->Mode == Profile->Mode);

			for (const FBlendProfileBoneEntry& Entry : Profile->ProfileEntries)
			{
				// Overlapping bone entries
				ensure(!MergedBlendProfile->ProfileEntries.ContainsByPredicate([Entry](const FBlendProfileBoneEntry& InEntry)
				{
					return InEntry.BoneReference.BoneName == Entry.BoneReference.BoneName;
				}));

				int32 BoneIndex = MergedBlendProfile->OwningSkeleton->GetReferenceSkeleton().FindBoneIndex(Entry.BoneReference.BoneName);
				if (BoneIndex == INDEX_NONE)
				{
					continue;
				}
				MergedBlendProfile->SetBoneBlendScale(Entry.BoneReference.BoneName, Entry.BlendScale, false, true);
			}
		}
	}
}

void UJrSkeletalMergingLibrary::AddAnimationSlotGroups(USkeleton* InSkeleton, const TMap<FName, TSet<FName>>& InSlotGroupsNames)
{
	for (const TPair<FName, TSet<FName>>& SlotGroupNamePair : InSlotGroupsNames)
	{
		InSkeleton->AddSlotGroupName(SlotGroupNamePair.Key);
		for (const FName& SlotName : SlotGroupNamePair.Value)
		{
			InSkeleton->SetSlotGroupName(SlotName, SlotGroupNamePair.Key);				
		}
	}
}
UE_ENABLE_OPTIMIZATION
#undef LOCTEXT_NAMESPACE