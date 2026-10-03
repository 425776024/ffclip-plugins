import { createTemplatePlayer, type TemplatePlayer } from './text';
import type { TextFrameRequest, TextWorkerReply } from './text-prefetch';
import { resolveRecipe } from '../text-wasm/src/recipes.mjs';

const scope = self as unknown as {
  postMessage(message: TextWorkerReply, transfer?: Transferable[]): void;
  onmessage: ((event: MessageEvent<{ id: number; input: TextFrameRequest }>) => void) | null;
};
let player: TemplatePlayer | undefined;
let playerKey = '';
let busy = false;

/** One worker owns one native template player; only its owned RGBA leaves the worker. */
scope.onmessage = async ({ data }) => {
  const { id, input } = data;
  if (busy) {
    scope.postMessage({
      id,
      error: { message: 'Native text worker already has an active request' }
    });
    return;
  }
  try {
    busy = true;
    const recipe = resolveRecipe(input.template);
    if (recipe.external || !['flower-style-03', 'flower-style-38'].includes(recipe.base))
      throw new Error('Native text prefetch supports only flower-style-03 and flower-style-38');
    const key = JSON.stringify([
      input.template,
      input.text,
      input.width,
      input.height,
      input.layout
    ]);
    if (!player || playerKey !== key) {
      player?.dispose();
      player = undefined;
      playerKey = '';
      const canvas = new OffscreenCanvas(input.width, input.height);
      player = await createTemplatePlayer(
        canvas,
        input.template,
        input.text,
        input.width,
        input.height,
        input.layout
      );
      playerKey = key;
    }
    const timeUs = Math.min(input.timeUs, Math.max(0, player.durationUs - 1));
    const result = await player.render(timeUs, { profile: true, present: false });
    if (!result.frame)
      throw new Error('Native flower text worker did not return its original RGBA frame');
    const frame = {
      ...result.frame,
      controlBounds: result.controlBounds,
      textLayout: result.textLayout
    };
    scope.postMessage({ id, frame }, [frame.data.buffer as ArrayBuffer]);
  } catch (error) {
    scope.postMessage({
      id,
      error: {
        message: error instanceof Error ? error.message : String(error),
        name: error instanceof Error ? error.name : 'Error',
        stack: error instanceof Error ? error.stack : undefined
      }
    });
  } finally {
    busy = false;
  }
};
