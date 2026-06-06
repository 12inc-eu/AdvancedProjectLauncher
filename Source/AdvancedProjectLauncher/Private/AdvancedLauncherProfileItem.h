// Copyright 12inc.eu Game Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ILauncherProfile.h"

/**
 * View-model for one row in the selectable profile list.
 *
 * Cached display strings are computed once at refresh time so the list rows do not recompute
 * them every paint. Selection (the checkbox) is user state that we preserve across refreshes.
 */
struct FAdvancedLauncherProfileItem
{
	/** The underlying launcher profile. */
	ILauncherProfilePtr Profile;

	/** Whether the user ticked this profile to be queued. */
	bool bSelected = false;

	/** Cached profile name. */
	FString Name;

	/** Cached one-line summary, e.g. "Tamed  |  Shipping". */
	FString Summary;

	/** IsValidForLaunch() evaluated at refresh time. Invalid profiles cannot be queued. */
	bool bValid = true;
};

using FAdvancedLauncherProfileItemPtr = TSharedPtr<FAdvancedLauncherProfileItem>;
