// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/DebugDrawQueue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"

#if CHAOS_DEBUG_DRAW
using namespace Chaos;

void FDebugDrawQueue::SetConsumerActive(void* Consumer, bool bConsumerActive)
{
	FScopeLock Lock(&ConsumersCS);

	if (bConsumerActive)
	{
		Consumers.AddUnique(Consumer);
	}
	else
	{
		Consumers.Remove(Consumer);
	}

	NumConsumers = Consumers.Num();
}

#endif
