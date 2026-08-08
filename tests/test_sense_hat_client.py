"""
tests/test_sense_hat_client.py -- Unit tests for sensehat_service.py
"""

import math
import sys
from unittest.mock import MagicMock

import pytest

# Mock sense_hat before importing sensehat_service (hardware-dependent lib).
sense_hat_mock = MagicMock()
sys.modules["sense_hat"] = sense_hat_mock

from em_server.services.sensehat_service import (
    _read_sense_hat,
    _read_sense_hat_with_guardrails,
)


class MockSenseHat:
    def __init__(self, temp_values=None, humidity=60.0, pressure=1013.0):
        self._temp_values = temp_values or [25.0, 25.5, 26.0]
        self._humidity = humidity
        self._pressure = pressure
        self.clear_called = False

    def get_temperature_from_humidity(self):
        return self._temp_values[0] if len(self._temp_values) > 0 else 25.0

    def get_temperature_from_pressure(self):
        return self._temp_values[1] if len(self._temp_values) > 1 else 25.5

    def get_temperature(self):
        return self._temp_values[2] if len(self._temp_values) > 2 else 26.0

    def get_humidity(self):
        return self._humidity

    def get_pressure(self):
        return self._pressure

    def clear(self):
        self.clear_called = True


class TestReadSenseHatWithGuardrails:

    def test_normal_readings(self):
        sense = MockSenseHat(temp_values=[25.0, 25.5, 26.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["temperature"]["value"] == 20.5
        assert result["temperature"]["unit"] == "°C"
        assert result["humidity"]["value"] == 60.0
        assert result["humidity"]["unit"] == "%"
        assert result["pressure"]["value"] == 1013.0
        assert result["pressure"]["unit"] == "hPa"

    def test_humidity_and_pressure_rounded(self):
        sense = MockSenseHat(temp_values=[25.0], humidity=60.123, pressure=1013.456)
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["humidity"]["value"] == 60.12
        assert result["pressure"]["value"] == 1013.46

    def test_temperature_out_of_range_no_fallback(self):
        sense = MockSenseHat(temp_values=[70.0, 71.0, 72.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert "temperature" not in result
        assert "humidity" in result
        assert "pressure" in result

    def test_temperature_out_of_range_with_fallback(self):
        sense = MockSenseHat(temp_values=[70.0, 71.0, 72.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
            last_valid_temperature_c=22.5,
        )
        assert result["temperature"]["value"] == 22.5

    def test_temperature_below_minimum(self):
        sense = MockSenseHat(temp_values=[-20.0, -19.0, -18.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert "temperature" not in result

    def test_temperature_below_minimum_with_fallback(self):
        sense = MockSenseHat(temp_values=[-20.0, -19.0, -18.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
            last_valid_temperature_c=24.0,
        )
        assert result["temperature"]["value"] == 24.0

    def test_all_temperature_getters_fail(self):
        sense = MockSenseHat()
        sense.get_temperature_from_humidity = lambda: (_ for _ in ()).throw(Exception("fail"))
        sense.get_temperature_from_pressure = lambda: (_ for _ in ()).throw(Exception("fail"))
        sense.get_temperature = lambda: (_ for _ in ()).throw(Exception("fail"))

        with pytest.raises(RuntimeError, match="No valid temperature readings"):
            _read_sense_hat_with_guardrails(
                sense,
                cpu_temp_correction=5.0,
                temp_min_c=-10.0,
                temp_max_c=65.0,
            )

    def test_non_finite_temperature_excluded(self):
        sense = MockSenseHat(temp_values=[math.nan, 25.5, math.inf])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["temperature"]["value"] == 20.5

    def test_custom_guardrails_parameters(self):
        sense = MockSenseHat(temp_values=[30.0, 31.0, 32.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=2.0,
            temp_min_c=0.0,
            temp_max_c=40.0,
        )
        assert result["temperature"]["value"] == 29.0

    def test_single_valid_temperature_getter(self):
        sense = MockSenseHat()
        sense.get_temperature_from_humidity = lambda: (_ for _ in ()).throw(Exception("fail"))
        sense.get_temperature_from_pressure = lambda: (_ for _ in ()).throw(Exception("fail"))
        sense.get_temperature = lambda: 30.0

        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["temperature"]["value"] == 25.0

    def test_temperature_at_exact_boundary_upper_kept(self):
        sense = MockSenseHat(temp_values=[70.0, 70.0, 70.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["temperature"]["value"] == 65.0

    def test_temperature_at_exact_boundary_lower_kept(self):
        sense = MockSenseHat(temp_values=[-5.0, -5.0, -5.0])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert result["temperature"]["value"] == -10.0

    def test_temperature_just_above_upper_discarded(self):
        sense = MockSenseHat(temp_values=[70.1, 70.1, 70.1])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert "temperature" not in result

    def test_temperature_just_below_lower_discarded(self):
        sense = MockSenseHat(temp_values=[-5.1, -5.1, -5.1])
        result = _read_sense_hat_with_guardrails(
            sense,
            cpu_temp_correction=5.0,
            temp_min_c=-10.0,
            temp_max_c=65.0,
        )
        assert "temperature" not in result


class TestReadSenseHat:

    def test_calls_with_default_guardrails(self):
        sense = MockSenseHat(temp_values=[25.0, 26.0, 27.0])
        result = _read_sense_hat(sense)
        assert result["temperature"]["value"] == 21.0
        assert result["humidity"]["value"] == 60.0
        assert result["pressure"]["value"] == 1013.0

    def test_temperature_out_of_range_defaults(self):
        sense = MockSenseHat(temp_values=[80.0, 81.0, 82.0])
        result = _read_sense_hat(sense)
        assert "temperature" not in result
