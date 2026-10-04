#pragma once

#include <utility>

template <typename T_ITR>
struct RangeWrapper
{
	RangeWrapper(T_ITR&& begin, T_ITR&& end)
		: Begin(std::forward<T_ITR>(begin))
		, End(std::forward<T_ITR>(end))
	{ }

	T_ITR begin() const { return Begin; }
	T_ITR end() const { return End; }

	T_ITR Begin;
	T_ITR End;
};


template <typename T_ITR>
RangeWrapper<T_ITR> make_iterator_range(T_ITR&& begin, T_ITR&& end) { return RangeWrapper<T_ITR>(std::forward<T_ITR>(begin), std::forward<T_ITR>(end)); }
