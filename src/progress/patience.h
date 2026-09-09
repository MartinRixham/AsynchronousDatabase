#ifndef PROGRESS_PATIENCE_H
#define PROGRESS_PATIENCE_H

#include <chrono>

namespace progress
{
	// A deadline a pass pushes forward whenever it gets somewhere, so what it bounds is time spent
	// getting nowhere rather than the whole of a pass.
	//
	// A pass that moves records has no wall clock that is right for it: how long it takes is how
	// much there is to move, and a store of a terabyte and a store of a megabyte are the same code.
	// A pass still being sent records is one to leave alone, and a pass being sent nothing is one to
	// stop with whatever it has managed.
	class patience
	{
		std::chrono::seconds without;

		std::chrono::steady_clock::time_point expires;

	public:
		explicit patience(long seconds);

		// Whether nothing has happened for long enough to stop asking.
		bool spent() const;

		// Something happened, so the waiting starts again.
		void renew();
	};
}

#endif
