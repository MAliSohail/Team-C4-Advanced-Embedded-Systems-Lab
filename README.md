# QC Station: Automated Mold Detection for Packaged Food

## What this is

This project is a small automated inspection station that checks food items for mold before they move further down a production or packaging line. A part is placed on a turntable, a camera photographs it from four angles, and each photo is checked by an image classification model trained to recognize mold. If any angle shows signs of mold, the item is rejected. If all four look clean, it passes.

The goal is to take a task that is usually done by a person glancing at a product and make it consistent, fast, and hands-off.

## Why this matters

Visual inspection for mold is a common step in food quality control, but doing it by eye has real limits. People get tired, get distracted, and see things differently from one shift to the next. Mold can also be easy to miss if it has only started forming on the underside or back of an item, since a quick glance usually only covers what is facing up.

Rotating the item and checking it from four separate angles is meant to close that gap. It also removes the inconsistency that comes with human judgment. This means the same item is very likely to get the same result whether it is the first inspection of the day or the last.

## What it does

- Detects when an item has been placed for inspection.
- Physically rotates the item to four fixed positions.
- Photographs the item at each position.
- Automatically classifies each photo as clean or moldy.
- Produces one final result for the item: PASS if every angle looks clean, REJECT if mold is detected at any angle.
- Flags the inspection as an error rather than guessing, if a camera, sensor, or connection fails partway through.
- Keeps a record of every inspection, including the photos and the result, so decisions can be reviewed later.

## What it does not do

This is a working prototype, not a certified food-safety system. It does not replace regulatory inspection processes, and it is not tuned for any specific food product, packaging type, or lighting environment out of the box. The classification model would need to be trained on real samples of the target food item before this could be trusted for anything beyond testing and demonstration.

It also inspects one item at a time. There is no conveyor integration, batching, or multi-item tracking. It is meant to prove out the approach, not to run a full production line.

## How it is put together

The station is made of a few physical and software pieces working together:

- **A turntable with a button**, so an operator can place an item and start an inspection with a single press.
- **A camera** that takes a photo each time the turntable reaches one of its four positions.
- **A master device** that coordinates everything: it tells the turntable where to move, tells the camera when to take a photo, runs each photo through the mold-detection model, and works out the final result.
- **A trained image classification model** that looks at a single photo and decides whether it shows mold. This runs directly on the onboard computer, so no internet connection or external service is needed to get a result.
- **A live dashboard**, viewable from any browser on the same network, that shows the current state of the inspection, the four photos as they come in, and the final result.

The pieces communicate over a local network, using a mix of direct requests and a lightweight messaging system built for this kind of device-to-device communication. This keeps each component simple and lets any single piece, like the camera or the turntable, be swapped out or upgraded without redesigning the whole system.

## Where this could go from here

A few natural next steps, if this were taken further:

- Training the model on a larger and more varied set of real food photos, to improve accuracy.
- Recovering an in-progress inspection automatically if the onboard computer restarts mid-run.
- Adding support for inspecting a continuous stream of items rather than one at a time.
- Logging longer-term trends, such as reject rates over time, to catch upstream problems earlier.
