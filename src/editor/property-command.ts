import { getPropertyDescriptor, validateProperty, type Item, type EditorCommand } from '../../packages/core/project.mjs';
import type { Interpolation, KeyframeProperty } from '../../packages/core/commands.js';

/** The dynamic Inspector crosses into the command union only after schema validation. */
export function propertyCommand(item: Item, property: string, value: unknown): Extract<EditorCommand, { action: 'set_property' }> {
  validateProperty(property, value, item.clip);
  return { action: 'set_property', itemId: item.id, property, value } as Extract<EditorCommand, { action: 'set_property' }>;
}

export function keyframeProperty(item: Item, property: string): KeyframeProperty {
  if (!getPropertyDescriptor(item, property)?.keyframe) throw new Error('此属性不支持关键帧');
  return property as KeyframeProperty;
}

export function keyframeCommand(item: Item, property: string, value: unknown, timeSeconds: number, interpolation: Interpolation = 'linear'): Extract<EditorCommand, { action: 'set_keyframe' }> {
  const path = keyframeProperty(item, property);
  validateProperty(property, value, item.clip);
  if (typeof value !== 'number') throw new Error('关键帧必须使用数值');
  return { action: 'set_keyframe', itemId: item.id, property: path, timeSeconds, value, interpolation };
}
