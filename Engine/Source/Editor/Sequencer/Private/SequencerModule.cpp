// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Textures/SlateIcon.h"
#include "EditorModeRegistry.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "IMovieRendererInterface.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "SequencerCommands.h"
#include "ISequencerObjectChangeListener.h"
#include "Sequencer.h"
#include "SequencerCustomizationManager.h"
#include "SequencerEdMode.h"
#include "SequencerObjectChangeListener.h"
#include "IDetailKeyframeHandler.h"
#include "Tree/CurveEditorTreeFilter.h"
#include "AnimatedPropertyKey.h"

#include "ToolMenus.h"
#include "ContentBrowserMenuContexts.h"
#include "SequencerUtilities.h"
#include "FileHelpers.h"
#include "LevelSequence.h"


#if !IS_MONOLITHIC
	UE::MovieScene::FEntityManager*& GEntityManagerForDebugging = UE::MovieScene::GEntityManagerForDebuggingVisualizers;
#endif



#define LOCTEXT_NAMESPACE "SequencerEditor"

// Destructor defined in CPP to avoid having to #include SequencerChannelInterface.h in the main module definition
ISequencerModule::~ISequencerModule()
{
}

ECurveEditorTreeFilterType ISequencerModule::GetSequencerSelectionFilterType()
{
	static ECurveEditorTreeFilterType FilterType = FCurveEditorTreeFilter::RegisterFilterType();
	return FilterType;
}

/**
 * SequencerModule implementation (private)
 */
class FSequencerModule
	: public ISequencerModule
{
public:

	// ISequencerModule interface

	virtual TSharedRef<ISequencer> CreateSequencer(const FSequencerInitParams& InitParams) override
	{
		TSharedRef<FSequencer> Sequencer = MakeShared<FSequencer>();
		TSharedRef<ISequencerObjectChangeListener> ObjectChangeListener = MakeShared<FSequencerObjectChangeListener>(Sequencer);

		OnPreSequencerInit.Broadcast(Sequencer, ObjectChangeListener, InitParams);

		Sequencer->InitSequencer(InitParams, ObjectChangeListener, TrackEditorDelegates, EditorObjectBindingDelegates);

		OnSequencerCreated.Broadcast(Sequencer);

		return Sequencer;
	}
	
	virtual FDelegateHandle RegisterTrackEditor( FOnCreateTrackEditor InOnCreateTrackEditor, TArrayView<FAnimatedPropertyKey> AnimatedPropertyTypes ) override
	{
		TrackEditorDelegates.Add( InOnCreateTrackEditor );
		FDelegateHandle Handle = TrackEditorDelegates.Last().GetHandle();
		for (const FAnimatedPropertyKey& Key : AnimatedPropertyTypes)
		{
			PropertyAnimators.Add(Key);
		}

		if (AnimatedPropertyTypes.Num() > 0)
		{
			FAnimatedTypeCache CachedTypes;
			CachedTypes.FactoryHandle = Handle;
			for (const FAnimatedPropertyKey& Key : AnimatedPropertyTypes)
			{
				CachedTypes.AnimatedTypes.Add(Key);
			}
			AnimatedTypeCache.Add(CachedTypes);
		}
		return Handle;
	}

	virtual void UnRegisterTrackEditor( FDelegateHandle InHandle ) override
	{
		TrackEditorDelegates.RemoveAll( [=](const FOnCreateTrackEditor& Delegate){ return Delegate.GetHandle() == InHandle; } );
		int32 CacheIndex = AnimatedTypeCache.IndexOfByPredicate([=](const FAnimatedTypeCache& In) { return In.FactoryHandle == InHandle; });
		if (CacheIndex != INDEX_NONE)
		{
			for (const FAnimatedPropertyKey& Key : AnimatedTypeCache[CacheIndex].AnimatedTypes)
			{
				PropertyAnimators.Remove(Key);
			}
			AnimatedTypeCache.RemoveAtSwap(CacheIndex);
		}
	}

	virtual FDelegateHandle RegisterOnSequencerCreated(FOnSequencerCreated::FDelegate InOnSequencerCreated) override
	{
		return OnSequencerCreated.Add(InOnSequencerCreated);
	}

	virtual void UnregisterOnSequencerCreated(FDelegateHandle InHandle) override
	{
		OnSequencerCreated.Remove(InHandle);
	}

	virtual FDelegateHandle RegisterOnPreSequencerInit(FOnPreSequencerInit::FDelegate InOnPreSequencerInit) override
	{
		return OnPreSequencerInit.Add(InOnPreSequencerInit);
	}

	virtual void UnregisterOnPreSequencerInit(FDelegateHandle InHandle) override
	{
		OnPreSequencerInit.Remove(InHandle);
	}

	virtual FDelegateHandle RegisterEditorObjectBinding(FOnCreateEditorObjectBinding InOnCreateEditorObjectBinding) override
	{
		EditorObjectBindingDelegates.Add(InOnCreateEditorObjectBinding);
		return EditorObjectBindingDelegates.Last().GetHandle();
	}

	virtual void UnRegisterEditorObjectBinding(FDelegateHandle InHandle) override
	{
		EditorObjectBindingDelegates.RemoveAll([=](const FOnCreateEditorObjectBinding& Delegate) { return Delegate.GetHandle() == InHandle; });
	}

	void RegisterMenus()
	{
		UToolMenus* ToolMenus = UToolMenus::Get();
		UToolMenu* Menu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu.LevelSequence");
		if (!Menu)
		{
			return;
		}

		FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
		Section.AddDynamicEntry("SequencerActions", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
			if (!Context)
			{
				return;
			}

			ULevelSequence* LevelSequence = Context->SelectedObjects.Num() == 1 ? Cast<ULevelSequence>(Context->SelectedObjects[0]) : nullptr;
			if (LevelSequence)
			{
				// if this LevelSequence has associated maps, offer to load them
				TArray<FString> AssociatedMaps = FSequencerUtilities::GetAssociatedMapPackages(LevelSequence);

				if(AssociatedMaps.Num()>0)
				{
					InSection.AddSubMenu(
						"SequencerOpenMap_Label",
						LOCTEXT("SequencerOpenMap_Label", "Open Map"),
						LOCTEXT("SequencerOpenMap_Tooltip", "Open a map associated with this Level Sequence Asset"),
						FNewMenuDelegate::CreateLambda(
							[AssociatedMaps](FMenuBuilder& SubMenuBuilder)
							{
								for (const FString& AssociatedMap : AssociatedMaps)
								{
									SubMenuBuilder.AddMenuEntry(
										FText::FromString(FPaths::GetBaseFilename(AssociatedMap)),
										FText(),
										FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Levels"),
										FExecuteAction::CreateLambda(
											[AssociatedMap]
											{
												FEditorFileUtils::LoadMap(AssociatedMap);
											}
										)
									);
								}
							}
						),
						false,
						FSlateIcon(FEditorStyle::GetStyleSetName(), "LevelEditor.Tabs.Levels")
					);
				}
			}
		}));
	}

	virtual void StartupModule() override
	{
		if (GIsEditor)
		{
			// EditorStyle must be initialized by now
			FModuleManager::Get().LoadModule("EditorStyle");
			FSequencerCommands::Register();

			FEditorModeRegistry::Get().RegisterMode<FSequencerEdMode>(
				FSequencerEdMode::EM_SequencerMode,
				NSLOCTEXT("Sequencer", "SequencerEditMode", "Sequencer Mode"),
				FSlateIcon(),
				false);

			if (UToolMenus::TryGet())
			{
				RegisterMenus();
			}
			else
			{
				FCoreDelegates::OnPostEngineInit.AddRaw(this, &FSequencerModule::RegisterMenus);
			}
		}

		ObjectBindingContextMenuExtensibilityManager = MakeShareable( new FExtensibilityManager );
		AddTrackMenuExtensibilityManager = MakeShareable( new FExtensibilityManager );
		ToolBarExtensibilityManager = MakeShareable(new FExtensibilityManager);

		SequencerCustomizationManager = MakeShareable(new FSequencerCustomizationManager);
	}

	virtual void ShutdownModule() override
	{
		if (GIsEditor)
		{
			FSequencerCommands::Unregister();

			FEditorModeRegistry::Get().UnregisterMode(FSequencerEdMode::EM_SequencerMode);
		}
	}

	virtual void RegisterPropertyAnimator(FAnimatedPropertyKey Key) override
	{
		PropertyAnimators.Add(Key);
	}

	virtual void UnRegisterPropertyAnimator(FAnimatedPropertyKey Key) override
	{
		PropertyAnimators.Remove(Key);
	}

	virtual bool CanAnimateProperty(FProperty* Property) override
	{
		if (PropertyAnimators.Contains(FAnimatedPropertyKey::FromProperty(Property)))
		{
			return true;
		}

		FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);

		// Check each level of the property hierarchy
		FFieldClass* PropertyType = Property->GetClass();
		while (PropertyType && PropertyType != FProperty::StaticClass())
		{
			FAnimatedPropertyKey Key = FAnimatedPropertyKey::FromPropertyTypeName(PropertyType->GetFName());

			// For object properties, check each parent type of the object (ie, so a track that animates UBaseClass ptrs can be used with a UDerivedClass property)
			UClass* ClassType = (ObjectProperty && ObjectProperty->PropertyClass) ? ObjectProperty->PropertyClass->GetSuperClass() : nullptr;
			while (ClassType)
			{
				Key.ObjectTypeName = ClassType->GetFName();
				if (PropertyAnimators.Contains(Key))
				{
					return true;
				}
				ClassType = ClassType->GetSuperClass();
			}

			Key.ObjectTypeName = NAME_None;
			if (PropertyAnimators.Contains(Key))
			{
				return true;
			}

			// Look at the property's super class
			PropertyType = PropertyType->GetSuperClass();
		}

		return false;
	}

	virtual TSharedPtr<FExtensibilityManager> GetObjectBindingContextMenuExtensibilityManager() const override { return ObjectBindingContextMenuExtensibilityManager; }
	virtual TSharedPtr<FExtensibilityManager> GetAddTrackMenuExtensibilityManager() const override { return AddTrackMenuExtensibilityManager; }
	virtual TSharedPtr<FExtensibilityManager> GetToolBarExtensibilityManager() const override { return ToolBarExtensibilityManager; }

	virtual TSharedPtr<FSequencerCustomizationManager> GetSequencerCustomizationManager() const override { return SequencerCustomizationManager; }


	virtual FDelegateHandle RegisterMovieRenderer(TUniquePtr<IMovieRendererInterface>&& InMovieRenderer) override
	{
		FDelegateHandle NewHandle(FDelegateHandle::GenerateNewHandle);
		MovieRenderers.Add(FMovieRendererEntry{ NewHandle, MoveTemp(InMovieRenderer) });
		return NewHandle;
	}

	virtual void UnregisterMovieRenderer(FDelegateHandle InDelegateHandle) override
	{
		MovieRenderers.RemoveAll([InDelegateHandle](const FMovieRendererEntry& In){ return In.Handle == InDelegateHandle; });
	}

	virtual IMovieRendererInterface* GetMovieRenderer(const FString& InMovieRendererName) override
	{
		for (const FMovieRendererEntry& MovieRenderer : MovieRenderers)
		{
			if (MovieRenderer.Renderer->GetDisplayName() == InMovieRendererName)
			{
				return MovieRenderer.Renderer.Get();
			}
		}

		return nullptr;
	}

	virtual TArray<FString> GetMovieRendererNames() override
	{
		TArray<FString> MovieRendererNames;
		for (const FMovieRendererEntry& MovieRenderer : MovieRenderers)
		{
			MovieRendererNames.Add(MovieRenderer.Renderer->GetDisplayName());
		}
		return MovieRendererNames;
	}

private:

	TSet<FAnimatedPropertyKey> PropertyAnimators;

	/** List of auto-key handler delegates sequencers will execute when they are created */
	TArray< FOnCreateTrackEditor > TrackEditorDelegates;

	/** List of object binding handler delegates sequencers will execute when they are created */
	TArray< FOnCreateEditorObjectBinding > EditorObjectBindingDelegates;

	/** Multicast delegate used to notify others of sequencer initialization params and allow modification. */
	FOnPreSequencerInit OnPreSequencerInit;

	/** Multicast delegate used to notify others of sequencer creations */
	FOnSequencerCreated OnSequencerCreated;

	struct FAnimatedTypeCache
	{
		FDelegateHandle FactoryHandle;
		TArray<FAnimatedPropertyKey, TInlineAllocator<4>> AnimatedTypes;
	};

	/** Map of all track editor factories to property types that they have registered to animated */
	TArray<FAnimatedTypeCache> AnimatedTypeCache;

	TSharedPtr<FExtensibilityManager> ObjectBindingContextMenuExtensibilityManager;
	TSharedPtr<FExtensibilityManager> AddTrackMenuExtensibilityManager;
	TSharedPtr<FExtensibilityManager> ToolBarExtensibilityManager;

	TSharedPtr<FSequencerCustomizationManager> SequencerCustomizationManager;

	struct FMovieRendererEntry
	{
		FDelegateHandle Handle;
		TUniquePtr<IMovieRendererInterface> Renderer;
	};

	/** Array of movie renderers */
	TArray<FMovieRendererEntry> MovieRenderers;
};

IMPLEMENT_MODULE(FSequencerModule, Sequencer);

#undef LOCTEXT_NAMESPACE
