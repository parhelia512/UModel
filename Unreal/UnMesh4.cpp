#include "Core.h"

#if UNREAL4

#include "UnrealClasses.h"
#include "UnMesh4.h"
#include "UnMesh3.h"		// for FSkeletalMeshLODInfo
#include "UnMeshTypes.h"
#include "UnMaterial3.h"

#include "SkeletalMesh.h"
#include "StaticMesh.h"
#include "TypeConvert.h"


//#define DEBUG_SKELMESH		1
//#define DEBUG_STATICMESH		1

#if DEBUG_SKELMESH
#define DBG_SKEL(...)			appPrintf(__VA_ARGS__);
#else
#define DBG_SKEL(...)
#endif

#if DEBUG_STATICMESH
#define DBG_STAT(...)			appPrintf(__VA_ARGS__);
#else
#define DBG_STAT(...)
#endif


#if NUM_INFLUENCES_UE4 != NUM_INFLUENCES
//!!#error NUM_INFLUENCES_UE4 and NUM_INFLUENCES are not matching!
#endif

#if MAX_SKELETAL_UV_SETS_UE4 > MAX_MESH_UV_SETS
#error MAX_SKELETAL_UV_SETS_UE4 too large
#endif

#if MAX_STATIC_UV_SETS_UE4 > MAX_MESH_UV_SETS
#error MAX_STATIC_UV_SETS_UE4 too large
#endif

#define TEXSTREAM_MAX_NUM_UVCHANNELS	4


/*-----------------------------------------------------------------------------
	USkeletalMesh
-----------------------------------------------------------------------------*/

struct FRecomputeTangentCustomVersion
{
	enum Type
	{
		BeforeCustomVersionWasAdded = 0,
		// UE4.12
		RuntimeRecomputeTangent = 1,
	};

	static int Get(const FArchive& Ar)
	{
		if (Ar.Game < GAME_UE4_12)
			return BeforeCustomVersionWasAdded;
		return RuntimeRecomputeTangent;
	}
};

struct FRigidVertex4
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[MAX_SKELETAL_UV_SETS_UE4];
	byte				BoneIndex;
	FColor				Color;

	friend FArchive& operator<<(FArchive &Ar, FRigidVertex4 &V)
	{
		Ar << V.Pos;
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];

		for (int i = 0; i < MAX_SKELETAL_UV_SETS_UE4; i++)
			Ar << V.UV[i];

		Ar << V.Color;
		Ar << V.BoneIndex;

		return Ar;
	}
};

struct FSoftVertex4
{
	FVector				Pos;
	FPackedNormal		Normal[3];
	FMeshUVFloat		UV[MAX_SKELETAL_UV_SETS_UE4];
	byte				BoneIndex[NUM_INFLUENCES_UE4];
	byte				BoneWeight[NUM_INFLUENCES_UE4];
	FColor				Color;

	friend FArchive& operator<<(FArchive &Ar, FSoftVertex4 &V)
	{
		int i;

		Ar << V.Pos;
		Ar << V.Normal[0] << V.Normal[1] << V.Normal[2];

		for (int i = 0; i < MAX_SKELETAL_UV_SETS_UE4; i++)
			Ar << V.UV[i];

		Ar << V.Color;

		if (Ar.ArVer >= VER_UE4_SUPPORT_8_BONE_INFLUENCES_SKELETAL_MESHES)
		{
			//!! todo: 8 influences will require BoneIndex[] and BoneWeight[] to have 8 items
			for (i = 0; i < NUM_INFLUENCES_UE4; i++) Ar << V.BoneIndex[i];
			Ar.Seek(Ar.Tell() + MAX_TOTAL_INFLUENCES_UE4 - NUM_INFLUENCES_UE4); // skip 8 influences info
			for (i = 0; i < NUM_INFLUENCES_UE4; i++) Ar << V.BoneWeight[i];
			Ar.Seek(Ar.Tell() + MAX_TOTAL_INFLUENCES_UE4 - NUM_INFLUENCES_UE4); // skip 8 influences info
		}
		else
		{
			// load 4 indices, put zeros to remaining 4 indices
			for (i = 0; i < 4; i++) Ar << V.BoneIndex[i];
			for (i = 0; i < 4; i++) Ar << V.BoneWeight[i];
			for (i = 4; i < NUM_INFLUENCES_UE4; i++) V.BoneIndex[i] = 0;		// TODO: loop from 4 to 4 (0 iterations)
			for (i = 4; i < NUM_INFLUENCES_UE4; i++) V.BoneWeight[i] = 0;
		}

		return Ar;
	}
};

struct FApexClothPhysToRenderVertData
{
	FVector4				PositionBaryCoordsAndDist;
	FVector4				NormalBaryCoordsAndDist;
	FVector4				TangentBaryCoordsAndDist;
	int16					SimulMeshVertIndices[4];
	int						Padding[2];

	friend FArchive& operator<<(FArchive& Ar, FApexClothPhysToRenderVertData& V)
	{
		Ar << V.PositionBaryCoordsAndDist << V.NormalBaryCoordsAndDist << V.TangentBaryCoordsAndDist;
		Ar << V.SimulMeshVertIndices[0] << V.SimulMeshVertIndices[1] << V.SimulMeshVertIndices[2] << V.SimulMeshVertIndices[3];
		Ar << V.Padding[0] << V.Padding[1];
		return Ar;
	}
};

struct FMeshUVChannelInfo
{
	bool					bInitialized;
	bool					bOverrideDensities;
	float					LocalUVDensities[TEXSTREAM_MAX_NUM_UVCHANNELS];

	friend FArchive& operator<<(FArchive& Ar, FMeshUVChannelInfo& V)
	{
		Ar << V.bInitialized << V.bOverrideDensities;
		for (int i = 0; i < TEXSTREAM_MAX_NUM_UVCHANNELS; i++)
		{
			Ar << V.LocalUVDensities[i];
		}
		return Ar;
	}
};

struct FSkeletalMaterial
{
	UMaterialInterface*		Material;
	bool					bEnableShadowCasting;
	FName					MaterialSlotName;
	FMeshUVChannelInfo		UVChannelData;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalMaterial& M)
	{
		Ar << M.Material;

		if (FEditorObjectVersion::Get(Ar) >= FEditorObjectVersion::RefactorMeshEditorMaterials)
		{
			Ar << M.MaterialSlotName;
			if (Ar.ContainsEditorData())
			{
				FName ImportedMaterialSlotName;
				Ar << ImportedMaterialSlotName;
			}
		}
		else
		{
			if (Ar.ArVer >= VER_UE4_MOVE_SKELETALMESH_SHADOWCASTING)
				Ar << M.bEnableShadowCasting;

			if (FRecomputeTangentCustomVersion::Get(Ar) >= FRecomputeTangentCustomVersion::RuntimeRecomputeTangent)
			{
				bool bRecomputeTangent;
				Ar << bRecomputeTangent;
			}
		}

		if (FRenderingObjectVersion::Get(Ar) >= FRenderingObjectVersion::TextureStreamingMeshUVChannelData)
		{
			Ar << M.UVChannelData;
		}

		return Ar;
	}
};

struct FSkelMeshSection4
{
	int16					MaterialIndex;
	int						BaseIndex;
	int						NumTriangles;
	byte					TriangleSorting;		// TEnumAsByte<ETriangleSortOption>
	bool					bDisabled;
	int16					CorrespondClothSectionIndex;

	// Data from FSkelMeshChunk, appeared in FSkelMeshSection after UE4.13
	int32					NumVertices;
	uint32					BaseVertexIndex;
	TArray<FSoftVertex4>	SoftVertices;			// editor-only data
	TArray<uint16>			BoneMap;
	int32					MaxBoneInfluences;
	bool					HasApexClothData;
	// UE4.14
	bool					bCastShadow;

	friend FArchive& operator<<(FArchive& Ar, FSkelMeshSection4& S)
	{
		guard(FSkelMeshSection4<<);

		FSkeletalMeshCustomVersion::Type SkelMeshVer = FSkeletalMeshCustomVersion::Get(Ar);

		FStripDataFlags StripFlags(Ar);
		Ar << S.MaterialIndex;

		if (SkelMeshVer < FSkeletalMeshCustomVersion::CombineSectionWithChunk)
		{
			int16 ChunkIndex;
			Ar << ChunkIndex;
		}

		if (!StripFlags.IsDataStrippedForServer())
		{
			Ar << S.BaseIndex;
			Ar << S.NumTriangles;
		}
		Ar << S.TriangleSorting;

		if (Ar.ArVer >= VER_UE4_APEX_CLOTH)
		{
			Ar << S.bDisabled;
			Ar << S.CorrespondClothSectionIndex;
		}

		if (Ar.ArVer >= VER_UE4_APEX_CLOTH_LOD)
		{
			byte bEnableClothLOD_DEPRECATED;
			Ar << bEnableClothLOD_DEPRECATED;
		}

		if (FRecomputeTangentCustomVersion::Get(Ar) >= FRecomputeTangentCustomVersion::RuntimeRecomputeTangent)
		{
			bool bRecomputeTangent;
			Ar << bRecomputeTangent;
		}

		if (FEditorObjectVersion::Get(Ar) >= FEditorObjectVersion::RefactorMeshEditorMaterials)
		{
			Ar << S.bCastShadow;
		}

		// UE4.13+ stuff
		S.HasApexClothData = false;
		if (SkelMeshVer >= FSkeletalMeshCustomVersion::CombineSectionWithChunk)
		{
			if (!StripFlags.IsDataStrippedForServer())
			{
				Ar << S.BaseVertexIndex;
			}
			if (!StripFlags.IsEditorDataStripped())
			{
				if (SkelMeshVer < FSkeletalMeshCustomVersion::CombineSoftAndRigidVerts)
				{
					TArray<FRigidVertex4> RigidVertices;
					Ar << RigidVertices;
					// these vertices should be converted to FSoftVertex4, but we're dropping this data anyway
				}
				Ar << S.SoftVertices;
			}
			Ar << S.BoneMap;
			if (SkelMeshVer >= FSkeletalMeshCustomVersion::SaveNumVertices)
				Ar << S.NumVertices;
			if (SkelMeshVer < FSkeletalMeshCustomVersion::CombineSoftAndRigidVerts)
			{
				int NumRigidVerts, NumSoftVerts;
				Ar << NumRigidVerts << NumSoftVerts;
			}
			Ar << S.MaxBoneInfluences;

			// Physics data, drop
			TArray<FApexClothPhysToRenderVertData> ApexClothMappingData;
			TArray<FVector> PhysicalMeshVertices;
			TArray<FVector> PhysicalMeshNormals;
			int16 CorrespondClothAssetIndex;
			int16 ClothAssetSubmeshIndex;

			Ar << ApexClothMappingData;
			Ar << PhysicalMeshVertices << PhysicalMeshNormals;
			Ar << CorrespondClothAssetIndex << ClothAssetSubmeshIndex;

			S.HasApexClothData = ApexClothMappingData.Num() > 0;
		}

		return Ar;

		unguard;
	}
};

struct FMultisizeIndexContainer
{
	TArray<uint16>			Indices16;
	TArray<uint32>			Indices32;

	FORCEINLINE bool Is32Bit() const
	{
		return (Indices32.Num() != 0);
	}

	// very similar to UE3 version (FSkelIndexBuffer3 in UnMesh3.cpp)
	//?? combine with UE3 version?
	friend FArchive& operator<<(FArchive& Ar, FMultisizeIndexContainer& B)
	{
		guard(FMultisizeIndexContainer<<);

		if (Ar.ArVer < VER_UE4_KEEP_SKEL_MESH_INDEX_DATA)
		{
			bool bOldNeedCPUAccess;
			Ar << bOldNeedCPUAccess;
		}
		byte DataSize;
		Ar << DataSize;

		if (DataSize == 2)
			B.Indices16.BulkSerialize(Ar);
		else if (DataSize == 4)
			B.Indices32.BulkSerialize(Ar);
		else
			appError("Unknown DataSize %d", DataSize);

		return Ar;

		unguard;
	}
};

struct FSkelMeshChunk4
{
	int						BaseVertexIndex;
	TArray<FRigidVertex4>	RigidVertices;		// editor-only data
	TArray<FSoftVertex4>	SoftVertices;		// editor-only data
	TArray<uint16>			BoneMap;
	int						NumRigidVertices;
	int						NumSoftVertices;
	int						MaxBoneInfluences;
	bool					HasApexClothData;

	friend FArchive& operator<<(FArchive& Ar, FSkelMeshChunk4& C)
	{
		guard(FSkelMeshChunk4<<);

		FSkeletalMeshCustomVersion::Type SkelMeshVer = FSkeletalMeshCustomVersion::Get(Ar);
		assert(SkelMeshVer < FSkeletalMeshCustomVersion::CombineSoftAndRigidVerts); // no more chunks since 4.13

		FStripDataFlags StripFlags(Ar);

		if (!StripFlags.IsDataStrippedForServer())
			Ar << C.BaseVertexIndex;

		if (!StripFlags.IsEditorDataStripped())
		{
			Ar << C.RigidVertices << C.SoftVertices;
		}

		Ar << C.BoneMap << C.NumRigidVertices << C.NumSoftVertices << C.MaxBoneInfluences;

		C.HasApexClothData = false;
		if (Ar.ArVer >= VER_UE4_APEX_CLOTH)
		{
			// Physics data, drop
			TArray<FApexClothPhysToRenderVertData> ApexClothMappingData;
			TArray<FVector> PhysicalMeshVertices;
			TArray<FVector> PhysicalMeshNormals;
			int16 CorrespondClothAssetIndex;
			int16 ClothAssetSubmeshIndex;

			Ar << ApexClothMappingData;
			Ar << PhysicalMeshVertices << PhysicalMeshNormals;
			Ar << CorrespondClothAssetIndex << ClothAssetSubmeshIndex;

			C.HasApexClothData = ApexClothMappingData.Num() > 0;
		}

		return Ar;

		unguard;
	}
};

static int GNumSkelUVSets = 1;
static int GNumSkelInfluences = 4;

struct FGPUVert4Common
{
	FPackedNormal		Normal[3];		// Normal[1] (TangentY) is reconstructed from other 2 normals
						//!! TODO: we're cutting down influences, but holding unused normal here!
						//!! Should rename Normal[] and split into 2 separate fields
	byte				BoneIndex[NUM_INFLUENCES_UE4];
	byte				BoneWeight[NUM_INFLUENCES_UE4];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert4Common &V)
	{
		Ar << V.Normal[0] << V.Normal[2];

		// Influences
		if (GNumSkelInfluences <= ARRAY_COUNT(V.BoneIndex))
		{
			for (int i = 0; i < GNumSkelInfluences; i++)
				Ar << V.BoneIndex[i];
			for (int i = 0; i < GNumSkelInfluences; i++)
				Ar << V.BoneWeight[i];
		}
		else
		{
			// possibly this vertex has more vertex influences
			assert(GNumSkelInfluences <= MAX_TOTAL_INFLUENCES_UE4);
			// serialize influences
			byte BoneIndex2[MAX_TOTAL_INFLUENCES_UE4];
			byte BoneWeight2[MAX_TOTAL_INFLUENCES_UE4];
			for (int i = 0; i < GNumSkelInfluences; i++)
				Ar << BoneIndex2[i];
			for (int i = 0; i < GNumSkelInfluences; i++)
				Ar << BoneWeight2[i];
			// check if sorting needed (possibly 2nd half of influences has zero weight)
			uint32 PackedWeight2 = * (uint32*) &BoneIndex2[4];
			if (PackedWeight2 != 0)
			{
//				printf("# %d %d %d %d %d %d %d %d\n", BoneWeight2[0],BoneWeight2[1],BoneWeight2[2],BoneWeight2[3],BoneWeight2[4],BoneWeight2[5],BoneWeight2[6],BoneWeight2[7]);
				// Here we assume than weights are sorted by value - didn't see the sorting code in UE4, but printing of data shows that.
				// Compute weight which should be distributed between other bones to keep sum of weights identity.
				int ExtraWeight = 0;
				for (int i = NUM_INFLUENCES_UE4; i < MAX_TOTAL_INFLUENCES_UE4; i++)
					ExtraWeight += BoneWeight2[i];
				int WeightPerBone = ExtraWeight / NUM_INFLUENCES_UE4; // note: could be division with remainder!
				for (int i = 0; i < NUM_INFLUENCES_UE4; i++)
				{
					BoneWeight2[i] += WeightPerBone;
					ExtraWeight -= WeightPerBone;
				}
				// add remaining weight to the first bone
				BoneWeight2[0] += ExtraWeight;
			}
			// copy influences to vertex
			for (int i = 0; i < NUM_INFLUENCES_UE4; i++)
			{
				V.BoneIndex[i] = BoneIndex2[i];
				V.BoneWeight[i] = BoneWeight2[i];
			}
		}
		return Ar;
	}
};

struct FGPUVert4Half : FGPUVert4Common
{
	FVector				Pos;
	FMeshUVHalf			UV[MAX_SKELETAL_UV_SETS_UE4];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert4Half &V)
	{
		Ar << *((FGPUVert4Common*)&V) << V.Pos;
		for (int i = 0; i < GNumSkelUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FGPUVert4Float : FGPUVert4Common
{
	FVector				Pos;
	FMeshUVFloat		UV[MAX_SKELETAL_UV_SETS_UE4];

	friend FArchive& operator<<(FArchive &Ar, FGPUVert4Float &V)
	{
		Ar << *((FGPUVert4Common*)&V) << V.Pos;
		for (int i = 0; i < GNumSkelUVSets; i++) Ar << V.UV[i];
		return Ar;
	}
};

struct FSkeletalMeshVertexBuffer4
{
	int						NumTexCoords;
	FVector					MeshExtension;		// not used in engine - there's no support for packed position (look for "FPackedPosition")
	FVector					MeshOrigin;			// ...
	bool					bUseFullPrecisionUVs;
	bool					bExtraBoneInfluences;
	TArray<FGPUVert4Half>	VertsHalf;
	TArray<FGPUVert4Float>	VertsFloat;


	friend FArchive& operator<<(FArchive& Ar, FSkeletalMeshVertexBuffer4& B)
	{
		guard(FSkeletalMeshVertexBuffer4<<);

		DBG_SKEL("VertexBuffer:\n");
		FStripDataFlags StripFlags(Ar, VER_UE4_STATIC_SKELETAL_MESH_SERIALIZATION_FIX);
		Ar << B.NumTexCoords << B.bUseFullPrecisionUVs;
		DBG_SKEL("  TC=%d FullPrecision=%d\n", B.NumTexCoords, B.bUseFullPrecisionUVs);

		if (Ar.ArVer >= VER_UE4_SUPPORT_GPUSKINNING_8_BONE_INFLUENCES)
		{
			Ar << B.bExtraBoneInfluences;
			DBG_SKEL("  ExtraInfs=%d\n", B.bExtraBoneInfluences);
		}

		Ar << B.MeshExtension << B.MeshOrigin;
		DBG_SKEL("  Ext=(%g %g %g) Org=(%g %g %g)\n", FVECTOR_ARG(B.MeshExtension), FVECTOR_ARG(B.MeshOrigin));

		// Serialize vertex data. Use global variables to avoid passing variables to serializers.
		GNumSkelUVSets = B.NumTexCoords;
		GNumSkelInfluences = B.bExtraBoneInfluences ? MAX_TOTAL_INFLUENCES_UE4 : NUM_INFLUENCES_UE4;
		if (!B.bUseFullPrecisionUVs)
			B.VertsHalf.BulkSerialize(Ar);
		else
			B.VertsFloat.BulkSerialize(Ar);
		DBG_SKEL("  Verts: Half[%d] Float[%d]\n", B.VertsHalf.Num(), B.VertsFloat.Num());

		return Ar;

		unguard;
	}

	inline int GetVertexCount() const
	{
		if (VertsHalf.Num()) return VertsHalf.Num();
		if (VertsFloat.Num()) return VertsFloat.Num();
		return 0;
	}
};

struct FSkeletalMeshVertexColorBuffer4
{
	TArray<FColor>				Data;

	friend FArchive& operator<<(FArchive& Ar, FSkeletalMeshVertexColorBuffer4& B)
	{
		guard(FSkeletalMeshVertexColorBuffer4<<);
		FStripDataFlags StripFlags(Ar, VER_UE4_STATIC_SKELETAL_MESH_SERIALIZATION_FIX);
		if (!StripFlags.IsDataStrippedForServer())
			B.Data.BulkSerialize(Ar);
		return Ar;
		unguard;
	}
};

struct FSkeletalMeshVertexAPEXClothBuffer
{
	// don't need physics - don't serialize any data, simply skip them
	friend FArchive& operator<<(FArchive& Ar, FSkeletalMeshVertexAPEXClothBuffer& B)
	{
		FStripDataFlags StripFlags(Ar, VER_UE4_STATIC_SKELETAL_MESH_SERIALIZATION_FIX);
		if (!StripFlags.IsDataStrippedForServer())
		{
			DBG_SKEL("Dropping ApexCloth\n");
			SkipBulkArrayData(Ar);
		}
		return Ar;
	}
};

struct FStaticLODModel4
{
	TArray<FSkelMeshSection4>	Sections;
	FMultisizeIndexContainer	Indices;
	FMultisizeIndexContainer	AdjacencyIndexBuffer;
	TArray<int16>				ActiveBoneIndices;
	TArray<int16>				RequiredBones;
	TArray<FSkelMeshChunk4>		Chunks;
	int							Size;
	int							NumVertices;
	int							NumTexCoords;
	FIntBulkData				RawPointIndices;
	TArray<int>					MeshToImportVertexMap;
	int							MaxImportVertex;
	FSkeletalMeshVertexBuffer4	VertexBufferGPUSkin;
	FSkeletalMeshVertexColorBuffer4 ColorVertexBuffer;
	FSkeletalMeshVertexAPEXClothBuffer APEXClothVertexBuffer;

	friend FArchive& operator<<(FArchive& Ar, FStaticLODModel4& Lod)
	{
		guard(FStaticLODModel4<<);

		FStripDataFlags StripFlags(Ar);

		Ar << Lod.Sections;
#if DEBUG_SKELMESH
		for (int i1 = 0; i1 < Lod.Sections.Num(); i1++)
		{
			FSkelMeshSection4 &S = Lod.Sections[i1];
			appPrintf("Sec[%d]: Mtl=%d, BaseIdx=%d, NumTris=%d\n", i1, S.MaterialIndex, S.BaseIndex, S.NumTriangles);
		}
#endif

		Ar << Lod.Indices;
		DBG_SKEL("Indices: %d (16) / %d (32)\n", Lod.Indices.Indices16.Num(), Lod.Indices.Indices32.Num());

		Ar << Lod.ActiveBoneIndices;
		DBG_SKEL("ActiveBones: %d\n", Lod.ActiveBoneIndices.Num());

		FSkeletalMeshCustomVersion::Type SkelMeshVer = FSkeletalMeshCustomVersion::Get(Ar);
		if (SkelMeshVer < FSkeletalMeshCustomVersion::CombineSectionWithChunk)
		{
			Ar << Lod.Chunks;
#if DEBUG_SKELMESH
			for (int i1 = 0; i1 < Lod.Chunks.Num(); i1++)
			{
				const FSkelMeshChunk4& C = Lod.Chunks[i1];
				appPrintf("Chunk[%d]: FirstVert=%d Rig=%d (%d), Soft=%d(%d), Bones=%d, MaxInf=%d\n", i1, C.BaseVertexIndex,
					C.RigidVertices.Num(), C.NumRigidVertices, C.SoftVertices.Num(), C.NumSoftVertices, C.BoneMap.Num(), C.MaxBoneInfluences);
			}
#endif
		}

		Ar << Lod.Size;
		if (!StripFlags.IsDataStrippedForServer())
			Ar << Lod.NumVertices;

		Ar << Lod.RequiredBones;
		DBG_SKEL("Size=%d, NumVerts=%d, RequiredBones=%d\n", Lod.Size, Lod.NumVertices, Lod.RequiredBones.Num());

		if (!StripFlags.IsEditorDataStripped())
			Lod.RawPointIndices.Skip(Ar);

		if (Ar.ArVer >= VER_UE4_ADD_SKELMESH_MESHTOIMPORTVERTEXMAP)
		{
			Ar << Lod.MeshToImportVertexMap << Lod.MaxImportVertex;
		}

		// geometry
		if (!StripFlags.IsDataStrippedForServer())
		{
			Ar << Lod.NumTexCoords;
			DBG_SKEL("TexCoords=%d\n", Lod.NumTexCoords);
			Ar << Lod.VertexBufferGPUSkin;

			USkeletalMesh4 *LoadingMesh = (USkeletalMesh4*)UObject::GLoadingObj;
			assert(LoadingMesh);
			if (LoadingMesh->bHasVertexColors)
			{
				appPrintf("WARNING: SkeletalMesh %s has vertex colors\n", LoadingMesh->Name);
				Ar << Lod.ColorVertexBuffer;
				DBG_SKEL("Colors: %d\n", Lod.ColorVertexBuffer.Data.Num());
			}

			if (Ar.ArVer < VER_UE4_REMOVE_EXTRA_SKELMESH_VERTEX_INFLUENCES)
			{
				appError("Unsupported: extra SkelMesh vertex influences (old mesh format)");
			}

			if (!StripFlags.IsClassDataStripped(1))
				Ar << Lod.AdjacencyIndexBuffer;

			if (Ar.ArVer >= VER_UE4_APEX_CLOTH && Lod.HasApexClothData())
				Ar << Lod.APEXClothVertexBuffer;
		}

		return Ar;

		unguard;
	}

	bool HasApexClothData() const
	{
		for (int i = 0; i < Chunks.Num(); i++)
			if (Chunks[i].HasApexClothData)			// pre-UE4.13 code
				return true;
		for (int i = 0; i < Sections.Num(); i++)	// UE4.13+
			if (Sections[i].HasApexClothData)
				return true;
		return false;
	}
};

USkeletalMesh4::USkeletalMesh4()
:	bHasVertexColors(false)
,	ConvertedMesh(NULL)
{}

USkeletalMesh4::~USkeletalMesh4()
{
	delete ConvertedMesh;
}

void USkeletalMesh4::Serialize(FArchive &Ar)
{
	guard(USkeletalMesh4::Serialize);

	Super::Serialize(Ar);

	FStripDataFlags StripFlags(Ar);

	Ar << Bounds;
	Ar << Materials;
#if DEBUG_SKELMESH
	for (int i1 = 0; i1 < Materials.Num(); i1++)
		appPrintf("Material[%d] = %s\n", i1, Materials[i1].Material ? Materials[i1].Material->Name : "None");
#endif

	Ar << RefSkeleton;
#if DEBUG_SKELMESH
	appPrintf("RefSkeleton: %d bones\n", RefSkeleton.RefBoneInfo.Num());
	for (int i1 = 0; i1 < RefSkeleton.RefBoneInfo.Num(); i1++)
		appPrintf("  [%d] n=%s p=%d\n", i1, *RefSkeleton.RefBoneInfo[i1].Name, RefSkeleton.RefBoneInfo[i1].ParentIndex);
#endif

	// serialize FSkeletalMeshResource (contains only array of FStaticLODModel objects)
	Ar << LODModels;

	DROP_REMAINING_DATA(Ar);

	ConvertMesh();

	unguard;
}


void USkeletalMesh4::ConvertMesh()
{
	guard(USkeletalMesh4::ConvertMesh);

	CSkeletalMesh *Mesh = new CSkeletalMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;		//?? UE3 meshes has radius 2 times larger than mesh
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// MeshScale, MeshOrigin, RotOrigin are removed in UE4
	//!! NOTE: MeshScale is integrated into RefSkeleton.RefBonePose[0].Scale3D.
	//!! Perhaps rotation/translation are integrated too!
	Mesh->MeshOrigin.Set(0, 0, 0);
	Mesh->RotOrigin.Set(0, 0, 0);
	Mesh->MeshScale.Set(1, 1, 1);							// missing in UE4

	// convert LODs
	Mesh->Lods.Empty(LODModels.Num());
	assert(LODModels.Num() == LODInfo.Num());
	for (int lod = 0; lod < LODModels.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticLODModel4 &SrcLod = LODModels[lod];

		int NumTexCoords = SrcLod.NumTexCoords;
		if (NumTexCoords > MAX_MESH_UV_SETS)
			appError("SkeletalMesh has %d UV sets", NumTexCoords);

		CSkelMeshLod *Lod = new (Mesh->Lods) CSkelMeshLod;
		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = true;
		Lod->HasTangents  = true;

		guard(ProcessVerts);

		// get vertex count and determine vertex source
		int VertexCount = SrcLod.VertexBufferGPUSkin.GetVertexCount();

		// allocate the vertices
		Lod->AllocateVerts(VertexCount);

		int chunkIndex = 0;
		const TArray<uint16>* BoneMap = NULL;
		int lastChunkVertex = -1;
		const FSkeletalMeshVertexBuffer4 &S = SrcLod.VertexBufferGPUSkin;
		CSkelMeshVertex *D = Lod->Verts;

		for (int Vert = 0; Vert < VertexCount; Vert++, D++)
		{
			if (Vert >= lastChunkVertex)
			{
				// proceed to next chunk or section
				// pre-UE4.13 code
				if (SrcLod.Chunks.Num())
				{
					const FSkelMeshChunk4& C = SrcLod.Chunks[chunkIndex++];
					lastChunkVertex = C.BaseVertexIndex + C.NumRigidVertices + C.NumSoftVertices;
					BoneMap = &C.BoneMap;
				}
				else
				{
					// UE4.13 has moved chunk information to sections
					const FSkelMeshSection4& S = SrcLod.Sections[chunkIndex++];
					lastChunkVertex = S.BaseVertexIndex + S.NumVertices;
					BoneMap = &S.BoneMap;
				}
			}

			// get vertex from GPU skin
			const FGPUVert4Common *V;		// has normal and influences, but no UV[] and position

			if (!S.bUseFullPrecisionUVs)
			{
				const FMeshUVHalf *SUV;
				const FGPUVert4Half &V0 = S.VertsHalf[Vert];
				D->Position = CVT(V0.Pos);
				V = &V0;
				SUV = V0.UV;
				// UV
				FMeshUVFloat fUV = SUV[0];				// convert half->float
				D->UV = CVT(fUV);
				for (int TexCoordIndex = 1; TexCoordIndex < NumTexCoords; TexCoordIndex++)
				{
					Lod->ExtraUV[TexCoordIndex-1][Vert] = CVT(SUV[TexCoordIndex]);
				}
			}
			else
			{
				const FMeshUVFloat *SUV;
				const FGPUVert4Float &V0 = S.VertsFloat[Vert];
				V = &V0;
				D->Position = CVT(V0.Pos);
				SUV = V0.UV;
				// UV
				FMeshUVFloat fUV = SUV[0];
				D->UV = CVT(fUV);
				for (int TexCoordIndex = 1; TexCoordIndex < NumTexCoords; TexCoordIndex++)
				{
					Lod->ExtraUV[TexCoordIndex-1][Vert] = CVT(SUV[TexCoordIndex]);
				}
			}
			// convert Normal[3]
			UnpackNormals(V->Normal, *D);
			// convert influences
//			int TotalWeight = 0;
			int i2 = 0;
			unsigned PackedWeights = 0;
			for (int i = 0; i < NUM_INFLUENCES_UE4; i++)
			{
				int BoneIndex  = V->BoneIndex[i];
				byte BoneWeight = V->BoneWeight[i];
				if (BoneWeight == 0) continue;				// skip this influence (but do not stop the loop!)
				PackedWeights |= BoneWeight << (i2 * 8);
				D->Bone[i2]   = (*BoneMap)[BoneIndex];
				i2++;
//				TotalWeight += BoneWeight;
			}
			D->PackedWeights = PackedWeights;
//			assert(TotalWeight == 255);
			if (i2 < NUM_INFLUENCES_UE4) D->Bone[i2] = INDEX_NONE; // mark end of list
		}

		unguard;	// ProcessVerts

		// indices
		Lod->Indices.Initialize(&SrcLod.Indices.Indices16, &SrcLod.Indices.Indices32);

		// sections
		guard(ProcessSections);
		Lod->Sections.Empty(SrcLod.Sections.Num());
		const FSkeletalMeshLODInfo &Info = LODInfo[lod];

		for (int Sec = 0; Sec < SrcLod.Sections.Num(); Sec++)
		{
			const FSkelMeshSection4 &S = SrcLod.Sections[Sec];
			CMeshSection *Dst = new (Lod->Sections) CMeshSection;

			// remap material for LOD
			int MaterialIndex = S.MaterialIndex;
			if (MaterialIndex >= 0 && MaterialIndex < Info.LODMaterialMap.Num())
				MaterialIndex = Info.LODMaterialMap[MaterialIndex];

			if (S.MaterialIndex < Materials.Num())
				Dst->Material = Materials[MaterialIndex].Material;
			Dst->FirstIndex = S.BaseIndex;
			Dst->NumFaces   = S.NumTriangles;
		}

		unguard;	// ProcessSections

		unguardf("lod=%d", lod); // ConvertLod
	}

	// copy skeleton
	guard(ProcessSkeleton);
	int NumBones = RefSkeleton.RefBoneInfo.Num();
	Mesh->RefSkeleton.Empty(NumBones);
	for (int i = 0; i < NumBones; i++)
	{
		const FMeshBoneInfo &B = RefSkeleton.RefBoneInfo[i];
		const FTransform    &T = RefSkeleton.RefBonePose[i];
		CSkelMeshBone *Dst = new (Mesh->RefSkeleton) CSkelMeshBone;
		Dst->Name        = B.Name;
		Dst->ParentIndex = B.ParentIndex;
		Dst->Position    = CVT(T.Translation);
		Dst->Orientation = CVT(T.Rotation);
		if (fabs(T.Scale3D.X - 1.0f) + fabs(T.Scale3D.Y - 1.0f) + fabs(T.Scale3D.Z - 1.0f) > 0.001f)
		{
			// TODO: mesh has non-identity scale
/*			if (i == 0)
			{
				// root bone
				Mesh->MeshScale = CVT(T.Scale3D); -- not works: should scale only the skeleton, but not geometry
			} */
			appPrintf("WARNING: Scale[%s] = %g %g %g\n", *B.Name, FVECTOR_ARG(T.Scale3D));
		}
		//!! use T.Scale3D
		// fix skeleton; all bones but 0
		if (i >= 1)
			Dst->Orientation.Conjugate();
	}
	unguard; // ProcessSkeleton

	Mesh->FinalizeMesh();

	unguard;
}


/*-----------------------------------------------------------------------------
	UStaticMesh
-----------------------------------------------------------------------------*/

UStaticMesh4::UStaticMesh4()
:	ConvertedMesh(NULL)
,	bUseHighPrecisionTangentBasis(false)
{}

UStaticMesh4::~UStaticMesh4()
{
	delete ConvertedMesh;
}


// Ambient occlusion data
// When changed, constant DISTANCEFIELD_DERIVEDDATA_VER TEXT is updated
struct FDistanceFieldVolumeData
{
	TArray<int16>	DistanceFieldVolume;	// TArray<Float16>
	FIntVector		Size;
	FBox			LocalBoundingBox;
	bool			bMeshWasClosed;
	bool			bBuiltAsIfTwoSided;
	bool			bMeshWasPlane;

	friend FArchive& operator<<(FArchive& Ar, FDistanceFieldVolumeData& V)
	{
		Ar << V.DistanceFieldVolume << V.Size << V.LocalBoundingBox << V.bMeshWasClosed;
		/// reference: 28.08.2014 - f5238f04
		if (Ar.ArVer >= VER_UE4_RENAME_CROUCHMOVESCHARACTERDOWN)
			Ar << V.bBuiltAsIfTwoSided;
		/// reference: 12.09.2014 - 890f1205
		if (Ar.ArVer >= VER_UE4_DEPRECATE_UMG_STYLE_ASSETS)
			Ar << V.bMeshWasPlane;
		return Ar;
	}
};


struct FStaticMeshSection4
{
	int				MaterialIndex;
	int				FirstIndex;
	int				NumTriangles;
	int				MinVertexIndex;
	int				MaxVertexIndex;
	bool			bEnableCollision;
	bool			bCastShadow;

	friend FArchive& operator<<(FArchive& Ar, FStaticMeshSection4& S)
	{
		Ar << S.MaterialIndex;
		Ar << S.FirstIndex << S.NumTriangles;
		Ar << S.MinVertexIndex << S.MaxVertexIndex;
		Ar << S.bEnableCollision << S.bCastShadow;
		return Ar;
	}
};


struct FPositionVertexBuffer4
{
	TArray<FVector>	Verts;
	int				Stride;
	int				NumVertices;

	friend FArchive& operator<<(FArchive& Ar, FPositionVertexBuffer4& S)
	{
		guard(FPositionVertexBuffer4<<);

		Ar << S.Stride << S.NumVertices;
		DBG_STAT("StaticMesh PositionStream: IS:%d NV:%d\n", S.Stride, S.NumVertices);
		S.Verts.BulkSerialize(Ar);
		return Ar;

		unguard;
	}
};


static int  GNumStaticUVSets   = 1;
static bool GUseStaticFloatUVs = true;
static bool GUseHighPrecisionTangents = false;

struct FStaticMeshUVItem4
{
	FPackedNormal	Normal[3];					//?? do we need 3 items here?
	FMeshUVFloat	UV[MAX_STATIC_UV_SETS_UE4];

	friend FArchive& operator<<(FArchive& Ar, FStaticMeshUVItem4& V)
	{
		if (!GUseHighPrecisionTangents)
		{
			Ar << V.Normal[0] << V.Normal[2];	// TangentX and TangentZ
		}
		else
		{
			FPackedRGBA16N Normal, Tangent;
			Ar << Normal << Tangent;
			V.Normal[0] = Normal.ToPackedNormal();
			V.Normal[2] = Tangent.ToPackedNormal();
		}

		if (GUseStaticFloatUVs)
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
				Ar << V.UV[i];
		}
		else
		{
			for (int i = 0; i < GNumStaticUVSets; i++)
			{
				// read in half format and convert to float
				FMeshUVHalf UVHalf;
				Ar << UVHalf;
				V.UV[i] = UVHalf;		// convert
			}
		}
		return Ar;
	}
};


struct FStaticMeshVertexBuffer4
{
	int				NumTexCoords;
	int				Stride;
	int				NumVertices;
	bool			bUseFullPrecisionUVs;
	bool			bUseHighPrecisionTangentBasis;
	TArray<FStaticMeshUVItem4> UV;

	friend FArchive& operator<<(FArchive& Ar, FStaticMeshVertexBuffer4& S)
	{
		guard(FStaticMeshVertexBuffer4<<);

		S.bUseHighPrecisionTangentBasis = false;

		FStripDataFlags StripFlags(Ar, VER_UE4_STATIC_SKELETAL_MESH_SERIALIZATION_FIX);
		Ar << S.NumTexCoords << S.Stride << S.NumVertices;
		Ar << S.bUseFullPrecisionUVs;
		if (Ar.ArVer >= VER_UE4_12)
		{
			Ar << S.bUseHighPrecisionTangentBasis;
		}
		GUseHighPrecisionTangents = S.bUseHighPrecisionTangentBasis;
		DBG_STAT("StaticMesh UV stream: TC:%d IS:%d NV:%d FloatUV:%d HQ_Tangent:%d\n", S.NumTexCoords, S.Stride, S.NumVertices, S.bUseFullPrecisionUVs, S.bUseHighPrecisionTangentBasis);

		if (!StripFlags.IsDataStrippedForServer())
		{
			GNumStaticUVSets = S.NumTexCoords;
			GUseStaticFloatUVs = S.bUseFullPrecisionUVs;
			S.UV.BulkSerialize(Ar);
		}

		return Ar;

		unguard;
	}
};


struct FColorVertexBuffer4
{
	int				Stride;
	int				NumVertices;
	TArray<FColor>	Data;

	friend FArchive& operator<<(FArchive& Ar, FColorVertexBuffer4& S)
	{
		guard(FColorVertexBuffer4<<);

		FStripDataFlags StripFlags(Ar, VER_UE4_STATIC_SKELETAL_MESH_SERIALIZATION_FIX);
		Ar << S.Stride << S.NumVertices;
		DBG_STAT("StaticMesh ColorStream: IS:%d NV:%d\n", S.Stride, S.NumVertices);
		if (!StripFlags.IsDataStrippedForServer() && (S.NumVertices > 0)) // zero size arrays are not serialized
			S.Data.BulkSerialize(Ar);
		return Ar;

		unguard;
	}
};


//!! if use this class for SkeletalMesh - use both DBG_STAT and DBG_SKEL
struct FRawStaticIndexBuffer4
{
	TArray<uint16>		Indices16;
	TArray<uint32>		Indices32;

	FORCEINLINE bool Is32Bit() const
	{
		return (Indices32.Num() != 0);
	}

	friend FArchive& operator<<(FArchive &Ar, FRawStaticIndexBuffer4 &S)
	{
		guard(FRawStaticIndexBuffer4<<);

		if (Ar.ArVer < VER_UE4_SUPPORT_32BIT_STATIC_MESH_INDICES)
		{
			S.Indices16.BulkSerialize(Ar);
			DBG_STAT("RawIndexBuffer, old format - %d indices\n", S.Indices16.Num());
		}
		else
		{
			// serialize all indices as byte array
			bool is32bit;
			TArray<byte> data;
			Ar << is32bit;
			data.BulkSerialize(Ar);
			DBG_STAT("RawIndexBuffer, 32 bit = %d, %d indices (data size = %d)\n", is32bit, data.Num() / (is32bit ? 4 : 2), data.Num());
			if (!data.Num()) return Ar;

			// convert data
			if (is32bit)
			{
				int count = data.Num() / 4;
				byte* src = &data[0];
				if (Ar.ReverseBytes)
					appReverseBytes(src, count, 4);
				S.Indices32.AddUninitialized(count);
				for (int i = 0; i < count; i++, src += 4)
					S.Indices32[i] = *(int*)src;
			}
			else
			{
				int count = data.Num() / 2;
				byte* src = &data[0];
				if (Ar.ReverseBytes)
					appReverseBytes(src, count, 2);
				S.Indices16.AddUninitialized(count);
				for (int i = 0; i < count; i++, src += 2)
					S.Indices16[i] = *(uint16*)src;
			}
		}

		return Ar;

		unguard;
	}
};


// FStaticMeshLODResources class (named differently here)
// NOTE: UE4 LOD models has no versioning code inside, versioning is performed before cooking, with constant STATICMESH_DERIVEDDATA_VER
// (it is changed in code to invalidate DerivedDataCache data)
struct FStaticMeshLODModel4
{
	TArray<FStaticMeshSection4> Sections;
	FStaticMeshVertexBuffer4 VertexBuffer;
	FPositionVertexBuffer4   PositionVertexBuffer;
	FColorVertexBuffer4      ColorVertexBuffer;
	FRawStaticIndexBuffer4   IndexBuffer;
	FRawStaticIndexBuffer4   ReversedIndexBuffer;
	FRawStaticIndexBuffer4   DepthOnlyIndexBuffer;
	FRawStaticIndexBuffer4   ReversedDepthOnlyIndexBuffer;
	FRawStaticIndexBuffer4   WireframeIndexBuffer;
	FRawStaticIndexBuffer4   AdjacencyIndexBuffer;
	float                    MaxDeviation;

	friend FArchive& operator<<(FArchive& Ar, FStaticMeshLODModel4 &Lod)
	{
		guard(FStaticMeshLODModel4<<);

		FStripDataFlags StripFlags(Ar);

		Ar << Lod.Sections;
#if DEBUG_STATICMESH
		appPrintf("%d sections\n", Lod.Sections.Num());
		for (int i = 0; i < Lod.Sections.Num(); i++)
		{
			FStaticMeshSection4 &S = Lod.Sections[i];
			appPrintf("  mat=%d firstIdx=%d numTris=%d firstVers=%d maxVert=%d\n", S.MaterialIndex, S.FirstIndex, S.NumTriangles,
				S.MinVertexIndex, S.MaxVertexIndex);
		}
#endif // DEBUG_STATICMESH

		Ar << Lod.MaxDeviation;

		if (!StripFlags.IsDataStrippedForServer())
		{
			Ar << Lod.PositionVertexBuffer;
			Ar << Lod.VertexBuffer;
			Ar << Lod.ColorVertexBuffer;
			Ar << Lod.IndexBuffer;
			if (Ar.ArVer >= VER_UE4_SOUND_CONCURRENCY_PACKAGE) Ar << Lod.ReversedIndexBuffer;
			Ar << Lod.DepthOnlyIndexBuffer;
			if (Ar.ArVer >= VER_UE4_SOUND_CONCURRENCY_PACKAGE) Ar << Lod.ReversedDepthOnlyIndexBuffer;
			/// reference for VER_UE4_SOUND_CONCURRENCY_PACKAGE:
			/// 25.09.2015 - 948c1698

			if (Ar.ArVer >= VER_UE4_FTEXT_HISTORY && Ar.ArVer < VER_UE4_RENAME_CROUCHMOVESCHARACTERDOWN)
			{
				/// reference:
				/// 03.06.2014 - 1464dcf2
				/// 28.08.2014 - f5238f04
				FDistanceFieldVolumeData DistanceFieldData;
				Ar << DistanceFieldData;
			}

			if (!StripFlags.IsEditorDataStripped())
				Ar << Lod.WireframeIndexBuffer;

			if (!StripFlags.IsClassDataStripped(1))
				Ar << Lod.AdjacencyIndexBuffer;
		}

		return Ar;
		unguard;
	}
};


struct FMeshSectionInfo
{
	int					MaterialIndex;
	bool				bEnableCollision;
	bool				bCastShadow;

	friend FArchive& operator<<(FArchive& Ar, FMeshSectionInfo& S)
	{
		return Ar << S.MaterialIndex << S.bEnableCollision << S.bCastShadow;
	}
};


struct FStaticMaterial
{
	UMaterialInterface* MaterialInterface;
	FName				MaterialSlotName;
	FMeshUVChannelInfo	UVChannelData;

	friend FArchive& operator<<(FArchive& Ar, FStaticMaterial& M)
	{
		Ar << M.MaterialInterface << M.MaterialSlotName;
		if (Ar.ContainsEditorData())
		{
			FName ImportedMaterialSlotName;
			Ar << ImportedMaterialSlotName;
		}
		if (FRenderingObjectVersion::Get(Ar) >= FRenderingObjectVersion::TextureStreamingMeshUVChannelData)
		{
			Ar << M.UVChannelData;
		}
		return Ar;
	}
};


void UStaticMesh4::Serialize(FArchive &Ar)
{
	guard(UStaticMesh4::Serialize);

	Super::Serialize(Ar);

	FStripDataFlags StripFlags(Ar);
	bool bCooked;
	Ar << bCooked;
	DBG_STAT("Serializing %s StaticMesh\n", bCooked ? "cooked" : "source");

	Ar << BodySetup;

	if (Ar.ArVer >= VER_UE4_STATIC_MESH_STORE_NAV_COLLISION)
		Ar << NavCollision;

	if (!StripFlags.IsEditorDataStripped())
	{
		if (Ar.ArVer < VER_UE4_DEPRECATED_STATIC_MESH_THUMBNAIL_PROPERTIES_REMOVED)
		{
			FRotator DummyThumbnailAngle;
			float DummyThumbnailDistance;
			Ar << DummyThumbnailAngle << DummyThumbnailDistance;
		}
		FString HighResSourceMeshName;
		unsigned HighResSourceMeshCRC;
		Ar << HighResSourceMeshName << HighResSourceMeshCRC;
	}

	Ar << LightingGuid;

	//!! TODO: support sockets
	Ar << Sockets;
	if (Sockets.Num()) appNotify("StaticMesh has %d sockets", Sockets.Num());

	// editor models
	if (!StripFlags.IsEditorDataStripped())
	{
		DBG_STAT("Serializing %d SourceModels\n", SourceModels.Num());
		for (int i = 0; i < SourceModels.Num(); i++)
		{
			// Serialize FRawMeshBulkData
			SourceModels[i].BulkData.Serialize(Ar);
			// drop extra fields
			FGuid Guid;
			bool bGuidIsHash;
			Ar << Guid << bGuidIsHash;
		}
		TMap<unsigned, FMeshSectionInfo> MeshSectionInfo;
		Ar << MeshSectionInfo;
	}

	// serialize FStaticMeshRenderData
	if (bCooked)
	{
		// Note: code below still contains 'if (bCooked)' switches, this is because the same
		// code could be used to read data from DDC, for non-cooked assets.
		DBG_STAT("Serializing RenderData\n");
		if (!bCooked)
		{
			TArray<int> WedgeMap;
			TArray<int> MaterialIndexToImportIndex;
			Ar << WedgeMap << MaterialIndexToImportIndex;
		}

		Ar << Lods; // original code: TArray<FStaticMeshLODResources> LODResources

		if (bCooked)
		{
			if (Ar.ArVer >= VER_UE4_RENAME_CROUCHMOVESCHARACTERDOWN)
			{
				/// reference: 28.08.2014 - f5238f04
				bool stripped = false;
				if (Ar.ArVer >= VER_UE4_RENAME_WIDGET_VISIBILITY)
				{
					/// reference: 13.11.2014 - 48a3c9b7
					FStripDataFlags StripFlags2(Ar);
					stripped = StripFlags.IsDataStrippedForServer();
				}
				if (!stripped)
				{
					// serialize FDistanceFieldVolumeData for each LOD
					for (int i = 0; i < Lods.Num(); i++)
					{
						bool HasDistanceDataField;
						Ar << HasDistanceDataField;
						if (HasDistanceDataField)
						{
							FDistanceFieldVolumeData VolumeData;
							Ar << VolumeData;
						}
					}
				}
			}
		}

		Ar << Bounds;
		Ar << bLODsShareStaticLighting;

		if (Ar.Game < GAME_UE4_14)
		{
			bool bReducedBySimplygon;
			Ar << bReducedBySimplygon;
		}

		if (FRenderingObjectVersion::Get(Ar) < FRenderingObjectVersion::TextureStreamingMeshUVChannelData)
		{
			float StreamingTextureFactors[MAX_STATIC_UV_SETS_UE4];
			float MaxStreamingTextureFactor;
			// StreamingTextureFactor for each UV set
			for (int i = 0; i < MAX_STATIC_UV_SETS_UE4; i++)
				Ar << StreamingTextureFactors[i];
			Ar << MaxStreamingTextureFactor;
		}

		if (bCooked)
		{
			// ScreenSize for each LOD
			int MaxNumLods = (Ar.Game >= GAME_UE4_9) ? MAX_STATIC_LODS_UE4 : 4;
			for (int i = 0; i < MaxNumLods; i++)
				Ar << ScreenSize[i];
		}
	} // end of FStaticMeshRenderData

	if (Ar.Game >= GAME_UE4_14)
	{
		// Serialize following data to obtain material references for UE4.14+.
		// Don't bother serializing anything beyond this point in earlier versions.
		bool bHasSpeedTreeWind;
		Ar << bHasSpeedTreeWind;
		if (bHasSpeedTreeWind)
		{
			//TODO - FSpeedTreeWind serialization
			DROP_REMAINING_DATA(Ar);
			char buf[1024];
			GetFullName(buf, 1024);
			appNotify("Dropping SpeedTree and material data for StaticMesh %s", buf);
			return;
		}
		if (FEditorObjectVersion::Get(Ar) >= FEditorObjectVersion::RefactorMeshEditorMaterials)
		{
			// UE4.14+ - "Materials" are deprecated, added StaticMaterials
			TArray<FStaticMaterial> StaticMaterials;
			Ar << StaticMaterials;
			// Copy StaticMaterials to Materials
			Materials.AddUninitialized(StaticMaterials.Num());
			for (int i = 0; i < StaticMaterials.Num(); i++)
				Materials[i] = StaticMaterials[i].MaterialInterface;
		}
	}

	// remaining is SpeedTree data
	DROP_REMAINING_DATA(Ar);

	if (bCooked)
		ConvertMesh();
	else
		ConvertSourceModels();

	unguard;
}


void UStaticMesh4::ConvertMesh()
{
	guard(UStaticMesh4::ConvertMesh);

	CStaticMesh *Mesh = new CStaticMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;			//?? UE3 meshes has radius 2 times larger than mesh itself; verifty for UE4
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// convert lods
	Mesh->Lods.Empty(Lods.Num());
	for (int lod = 0; lod < Lods.Num(); lod++)
	{
		guard(ConvertLod);

		const FStaticMeshLODModel4 &SrcLod = Lods[lod];
		CStaticMeshLod *Lod = new (Mesh->Lods) CStaticMeshLod;

		int NumTexCoords = SrcLod.VertexBuffer.NumTexCoords;
		int NumVerts     = SrcLod.PositionVertexBuffer.Verts.Num();

		if (NumTexCoords > MAX_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", NumTexCoords);

		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = true;
		Lod->HasTangents  = true;

		// sections
		Lod->Sections.AddDefaulted(SrcLod.Sections.Num());
		for (int i = 0; i < SrcLod.Sections.Num(); i++)
		{
			CMeshSection &Dst = Lod->Sections[i];
			const FStaticMeshSection4 &Src = SrcLod.Sections[i];
			if (Materials.IsValidIndex(Src.MaterialIndex))
				Dst.Material = (UUnrealMaterial*)Materials[Src.MaterialIndex];
			Dst.FirstIndex = Src.FirstIndex;
			Dst.NumFaces   = Src.NumTriangles;
		}

		// vertices
		Lod->AllocateVerts(NumVerts);
		for (int i = 0; i < NumVerts; i++)
		{
			const FStaticMeshUVItem4 &SUV = SrcLod.VertexBuffer.UV[i];
			CStaticMeshVertex &V = Lod->Verts[i];

			V.Position = CVT(SrcLod.PositionVertexBuffer.Verts[i]);
			UnpackNormals(SUV.Normal, V);
			// copy UV
			const FMeshUVFloat* fUV = &SUV.UV[0];
			V.UV = *CVT(fUV);
			for (int TexCoordIndex = 1; TexCoordIndex < NumTexCoords; TexCoordIndex++)
			{
				fUV++;
				Lod->ExtraUV[TexCoordIndex-1][i] = *CVT(fUV);
			}
			//!! also has ColorStream
		}

		// indices
		Lod->Indices.Initialize(&SrcLod.IndexBuffer.Indices16, &SrcLod.IndexBuffer.Indices32);
		if (Lod->Indices.Num() == 0) appError("This StaticMesh doesn't have an index buffer");

		unguardf("lod=%d", lod);
	}

	Mesh->FinalizeMesh();

	unguard;
}


struct FRawMesh
{
	TArray<int>			FaceMaterialIndices;
	TArray<int>			FaceSmoothingMask;
	TArray<FVector>		VertexPositions;
	TArray<int>			WedgeIndices;
	TArray<FVector>		WedgeTangent;
	TArray<FVector>		WedgeBinormal;
	TArray<FVector>		WedgeNormal;
	TArray<FVector2D>	WedgeTexCoords[MAX_STATIC_UV_SETS_UE4];
	TArray<FColor>		WedgeColors;
	TArray<int>			MaterialIndexToImportIndex;

	void Serialize(FArchive& Ar)
	{
		guard(FRawMesh::Serialize);

		int Version, LicenseeVersion;
		Ar << Version << LicenseeVersion;

		Ar << FaceMaterialIndices;
		Ar << FaceSmoothingMask;
		Ar << VertexPositions;
		Ar << WedgeIndices;
		Ar << WedgeTangent;
		Ar << WedgeBinormal;
		Ar << WedgeNormal;
		for (int i = 0; i < MAX_STATIC_UV_SETS_UE4; i++)
			Ar << WedgeTexCoords[i];
		Ar << WedgeColors;

		if (Version >= 1) // RAW_MESH_VER_REMOVE_ZERO_TRIANGLE_SECTIONS
			Ar << MaterialIndexToImportIndex;

		unguard;
	}
};


void UStaticMesh4::ConvertSourceModels()
{
	guard(UStaticMesh4::ConvertSourceModels);

	CStaticMesh *Mesh = new CStaticMesh(this);
	ConvertedMesh = Mesh;

	// convert bounds
	// (note: copy-paste of ConvertedMesh's code)
	Mesh->BoundingSphere.R = Bounds.SphereRadius / 2;			//?? UE3 meshes has radius 2 times larger than mesh itself; verifty for UE4
	VectorSubtract(CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Min));
	VectorAdd     (CVT(Bounds.Origin), CVT(Bounds.BoxExtent), CVT(Mesh->BoundingBox.Max));

	// convert lods
	Mesh->Lods.Empty(Lods.Num());

	for (int LODIndex = 0; LODIndex < SourceModels.Num(); LODIndex++)
	{
		guard(ConvertLod);

		const FStaticMeshSourceModel& SrcModel = SourceModels[LODIndex];
		const FByteBulkData& Bulk = SrcModel.BulkData;
		if (Bulk.ElementCount == 0) continue;	// this SourceModel has generated LOD, not imported one

		CStaticMeshLod *Lod = new (Mesh->Lods) CStaticMeshLod;

		FRawMesh RawMesh;
		FMemReader Reader(Bulk.BulkData, Bulk.ElementCount); // ElementCount is the same as data size, for byte bulk data
		Reader.SetupFrom(*GetPackageArchive());
		RawMesh.Serialize(Reader);

		int NumTexCoords = MAX_STATIC_UV_SETS_UE4;
		for (int i = 0; i < MAX_STATIC_UV_SETS_UE4; i++)
		{
			if (!RawMesh.WedgeTexCoords[i].Num())
			{
				NumTexCoords = i;
				break;
			}
		}

		if (NumTexCoords > MAX_MESH_UV_SETS)
			appError("StaticMesh has %d UV sets", NumTexCoords);

		Lod->NumTexCoords = NumTexCoords;
		Lod->HasNormals   = !SrcModel.BuildSettings.bRecomputeNormals && (RawMesh.WedgeNormal.Num() > 0);
		Lod->HasTangents  = !SrcModel.BuildSettings.bRecomputeTangents && (RawMesh.WedgeTangent.Num() > 0) && (RawMesh.WedgeBinormal.Num() > 0)
							&& Lod->HasNormals;
		//!! TODO: should use FaceSmoothingMask for recomputing normals

		int PrevMaterialIndex = -1;
		for (int i = 0; i < RawMesh.FaceMaterialIndices.Num(); i++)
		{
			int MaterialIndex = RawMesh.FaceMaterialIndices[i];
			// We're not performing UE4-like mesh build, where multiple sections with the same
			// material will be combined into a single one. Instead, we're making a separate
			// section in that case.
			if (MaterialIndex != PrevMaterialIndex)
			{
				PrevMaterialIndex = MaterialIndex;
				CMeshSection* Sec = new (Lod->Sections) CMeshSection;
				if (Materials.IsValidIndex(MaterialIndex))
					Sec->Material = (UUnrealMaterial*)Materials[MaterialIndex];
				Sec->FirstIndex = i * 3;
			}
		}
		// Count face count per section
		for (int i = 0; i < Lod->Sections.Num(); i++)
		{
			CMeshSection& Sec = Lod->Sections[i];
			if (i < Lod->Sections.Num() - 1)
				Sec.NumFaces = (Lod->Sections[i+1].FirstIndex - Sec.FirstIndex) / 3;
			else
				Sec.NumFaces = RawMesh.FaceMaterialIndices.Num() - Sec.FirstIndex / 3;
		}

		// vertices
		int NumVerts = RawMesh.WedgeIndices.Num();
		Lod->AllocateVerts(NumVerts);
		assert(NumTexCoords >= 1);

		for (int i = 0; i < NumVerts; i++)
		{
			CStaticMeshVertex &V = Lod->Verts[i];

			int PositionIndex = RawMesh.WedgeIndices[i];
			V.Position = CVT(RawMesh.VertexPositions[PositionIndex]);
			// Pack normals
			if (Lod->HasNormals)
			{
				CVec3 Normal = CVT(RawMesh.WedgeNormal[i]);
				if (Lod->HasTangents)
				{
					CVec3 Tangent = CVT(RawMesh.WedgeTangent[i]);
					CVec3 Binormal = CVT(RawMesh.WedgeBinormal[i]);
					Pack(V.Normal, Normal);
					Pack(V.Tangent, Tangent);
					CVec3 ComputedBinormal;
					cross(Normal, Tangent, ComputedBinormal);
					float Sign = dot(Binormal, ComputedBinormal);
					V.Normal.SetW(Sign > 0 ? 1.0f : -1.0f);
				}
			}

			// copy UV
			V.UV = CVT(RawMesh.WedgeTexCoords[0][i]);
			for (int TexCoordIndex = 1; TexCoordIndex < NumTexCoords; TexCoordIndex++)
			{
				Lod->ExtraUV[TexCoordIndex-1][i] = CVT(RawMesh.WedgeTexCoords[TexCoordIndex][i]);
			}
			//!! also has ColorStream
		}

		// indices
		TArray<unsigned>& Indices32 = Lod->Indices.Indices32;
		Indices32.AddUninitialized(NumVerts);
		for (int i = 0; i < NumVerts; i++)
			Indices32[i] = i;

		unguardf("lod=%d", LODIndex);
	}

	Mesh->FinalizeMesh();

	unguard;
}


#endif // UNREAL4
