/*
 * The fence of one submission, for a caller that waits for it on a thread of its own: Metal's
 * -commit returns at once and the command buffer completes later, so the driver keeps the fence
 * of each -commit and its completion thread waits for it without the library's lock, which
 * rwa_finish holds for the whole wait.
 */
struct rwa_fence *
rwa_fence_take(struct rwa_device *device)
{
   RWA_LOCKED;
   struct pipe_fence_handle *fence = NULL;

   if (device && device->pending)
      device->screen->fence_reference(device->screen, &fence, device->pending);
   return (struct rwa_fence *)fence;
}

/* 0 once the GPU has done the submission (or there was none), -1 after RWA_FENCE_TIMEOUT_NS. */
int
rwa_fence_wait(struct rwa_device *device, struct rwa_fence *fence)
{
   if (!device || !fence)
      return 0;
   return device->screen->fence_finish(device->screen, NULL, (struct pipe_fence_handle *)fence,
                                       RWA_FENCE_TIMEOUT_NS) ? 0 : -1;
}

void
rwa_fence_release(struct rwa_device *device, struct rwa_fence *fence)
{
   struct pipe_fence_handle *handle = (struct pipe_fence_handle *)fence;

   if (device && handle)
      device->screen->fence_reference(device->screen, &handle, NULL);
}
