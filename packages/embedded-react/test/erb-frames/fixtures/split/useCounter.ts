import {useCallback, useState} from 'react';

export const STEP = 3;

export function useCounter(limit: number) {
  const [count, setCount] = useState(0);
  const bump = useCallback(
    () => setCount(previous => (previous + 1) % limit),
    [limit],
  );
  return {count, bump};
}
